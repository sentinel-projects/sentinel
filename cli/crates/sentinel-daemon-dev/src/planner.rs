//! Bounded structured planner decisions, based on LlmAgentRuntime and AgentLoop.
//! Parsing a final decision does not authorize, ground, or complete a run.
use crate::{
    observation::Grounding,
    tool_registry::{self, Invocation},
};
use serde_json::Value;

pub struct Claim {
    pub id: String,
    pub assertion: bool,
}
pub struct ToolCall {
    pub invocation: Invocation,
    pub depends_on: Vec<usize>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallState {
    Pending,
    Running,
    Succeeded,
    Failed,
    Blocked,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScheduleError {
    Invalid,
    OutOfScope,
    Repeated,
    Busy,
}
/// Sequential scheduler: validation/scope precede every audit or side effect.
pub struct BatchSchedule<'a> {
    calls: &'a [ToolCall],
    states: Vec<CallState>,
    dependencies: Vec<Vec<usize>>,
    cancelled: bool,
}
impl<'a> BatchSchedule<'a> {
    pub fn new(
        calls: &'a [ToolCall],
        mut in_scope: impl FnMut(&Invocation) -> bool,
    ) -> Result<Self, ScheduleError> {
        if calls.is_empty() || calls.len() > 8 {
            return Err(ScheduleError::Invalid);
        }
        let mut seen = std::collections::HashSet::new();
        for (index, call) in calls.iter().enumerate() {
            if !call.invocation.is_current() || call.depends_on.iter().any(|dep| *dep >= index) {
                return Err(ScheduleError::Invalid);
            }
            if !in_scope(&call.invocation) {
                return Err(ScheduleError::OutOfScope);
            }
            if !seen.insert(action_key(&call.invocation)) {
                return Err(ScheduleError::Repeated);
            }
        }
        Ok(Self {
            calls,
            states: vec![CallState::Pending; calls.len()],
            dependencies: calls.iter().map(|call| call.depends_on.clone()).collect(),
            cancelled: false,
        })
    }
    pub fn next(&mut self) -> Result<Option<usize>, ScheduleError> {
        if self.states.contains(&CallState::Running) {
            return Err(ScheduleError::Busy);
        }
        if self.cancelled {
            return Ok(None);
        }
        for index in 0..self.states.len() {
            if self.states[index] != CallState::Pending {
                continue;
            }
            if self.dependencies[index]
                .iter()
                .any(|dep| matches!(self.states[*dep], CallState::Failed | CallState::Blocked))
            {
                self.states[index] = CallState::Blocked;
                continue;
            }
            if self.dependencies[index]
                .iter()
                .all(|dep| self.states[*dep] == CallState::Succeeded)
            {
                self.states[index] = CallState::Running;
                return Ok(Some(index));
            }
        }
        Ok(None)
    }
    pub fn finish(&mut self, index: usize, success: bool) -> Result<(), ScheduleError> {
        let state = self.states.get_mut(index).ok_or(ScheduleError::Invalid)?;
        if *state != CallState::Running {
            return Err(ScheduleError::Invalid);
        }
        *state = if success {
            CallState::Succeeded
        } else {
            CallState::Failed
        };
        Ok(())
    }
    pub fn states(&self) -> &[CallState] {
        &self.states
    }
    pub fn call(&self, index: usize) -> Option<&ToolCall> {
        self.calls.get(index)
    }
    /// Prevents new dispatches; the runtime must separately cancel an in-flight call.
    pub fn cancel(&mut self) {
        self.cancelled = true;
    }
}

fn action_key(invocation: &Invocation) -> String {
    let mut args = invocation.arguments().clone();
    if let Some(path) = args.get("path").and_then(Value::as_str) {
        args["path"] = Value::String(crate::observation::normalized_resource(
            path,
            crate::observation::Domain::FileSystem,
        ));
    }
    format!("{}|{}", invocation.descriptor().id, args)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RepeatReason {
    Immediate,
    PriorVerified,
    FailedCycle,
}
struct PriorAction {
    key: String,
    succeeded: bool,
    verified: bool,
}
#[derive(Default)]
pub struct RepeatGuard {
    history: Vec<PriorAction>,
}
impl RepeatGuard {
    pub fn check(&self, invocation: &Invocation) -> Option<RepeatReason> {
        let key = action_key(invocation);
        if self.history.last().is_some_and(|last| last.key == key) {
            return Some(RepeatReason::Immediate);
        }
        if self
            .history
            .iter()
            .rev()
            .any(|item| item.key == key && item.succeeded && item.verified)
        {
            return Some(RepeatReason::PriorVerified);
        }
        if self.history.len() >= 4 {
            let tail = &self.history[self.history.len() - 4..];
            if tail.iter().all(|item| !item.succeeded)
                && tail[0].key != tail[1].key
                && tail[0].key == tail[2].key
                && tail[1].key == tail[3].key
                && tail[0].key == key
            {
                return Some(RepeatReason::FailedCycle);
            }
        }
        None
    }
    /// `verified` must come from actual evidence; successful mutation invalidates prior reads.
    pub fn record(&mut self, invocation: &Invocation, succeeded: bool, verified: bool) {
        if succeeded && invocation.descriptor().access == "write" {
            for item in &mut self.history {
                item.verified = false;
            }
        }
        if self.history.len() >= 64 {
            self.history.remove(0);
        }
        self.history.push(PriorAction {
            key: action_key(invocation),
            succeeded,
            verified,
        });
    }
}
pub enum Decision {
    ToolCall {
        calls: Vec<ToolCall>,
        requires_observation: Option<bool>,
    },
    Final {
        answer: String,
        grounding: Grounding,
        claims: Vec<Claim>,
        requires_observation: Option<bool>,
    },
    GiveUp {
        reason: String,
        requires_observation: Option<bool>,
    },
}
#[derive(Debug, PartialEq, Eq)]
pub enum ParseError {
    Invalid,
    TooLarge,
    UnknownTool,
    InvalidArguments,
}
const MAX_RESPONSE_BYTES: usize = 64 * 1024;
const MAX_ARGS_BYTES: usize = 16 * 1024;

pub fn parse(text: &str) -> Result<Decision, ParseError> {
    if text.len() > MAX_RESPONSE_BYTES {
        return Err(ParseError::TooLarge);
    }
    let mut text = text.trim();
    if let Some(fenced) = text
        .strip_prefix("```json\n")
        .and_then(|s| s.strip_suffix("\n```"))
    {
        text = fenced.trim();
    }
    if !text.starts_with('{') || !text.ends_with('}') {
        return Err(ParseError::Invalid);
    }
    let value: Value = serde_json::from_str(text).map_err(|_| ParseError::Invalid)?;
    let object = value.as_object().ok_or(ParseError::Invalid)?;
    let declared = match object.get("requiresObservation") {
        None => None,
        Some(v) => Some(v.as_bool().ok_or(ParseError::Invalid)?),
    };
    let action = object
        .get("action")
        .and_then(Value::as_str)
        .ok_or(ParseError::Invalid)?
        .to_lowercase();
    match action.as_str() {
        "final" => {
            let answer = object
                .get("answer")
                .and_then(Value::as_str)
                .unwrap_or("")
                .to_owned();
            let grounding = match object.get("grounding").and_then(Value::as_str) {
                Some("verified") => Grounding::Verified,
                Some("unable_to_verify") => Grounding::UnableToVerify,
                Some("context") => Grounding::Context,
                _ => return Err(ParseError::Invalid),
            };
            if answer.trim().is_empty() && grounding != Grounding::Verified {
                return Err(ParseError::Invalid);
            }
            let claims = match object.get("claims") {
                None => Vec::new(),
                Some(Value::Array(items)) => {
                    if items.len() > 64 {
                        return Err(ParseError::TooLarge);
                    }
                    items
                        .iter()
                        .map(|v| {
                            Ok(Claim {
                                id: v
                                    .get("id")
                                    .and_then(Value::as_str)
                                    .filter(|s| !s.is_empty() && s.len() <= 256)
                                    .ok_or(ParseError::Invalid)?
                                    .to_owned(),
                                assertion: v
                                    .get("assertion")
                                    .and_then(Value::as_bool)
                                    .ok_or(ParseError::Invalid)?,
                            })
                        })
                        .collect::<Result<Vec<_>, ParseError>>()?
                }
                _ => return Err(ParseError::Invalid),
            };
            Ok(Decision::Final {
                answer,
                grounding,
                claims,
                requires_observation: declared,
            })
        }
        "giveup" => {
            let reason = object
                .get("reason")
                .and_then(Value::as_str)
                .filter(|s| !s.trim().is_empty())
                .ok_or(ParseError::Invalid)?
                .to_owned();
            Ok(Decision::GiveUp {
                reason,
                requires_observation: declared,
            })
        }
        "tool_batch" => {
            let encoded = object.get("callsJson").and_then(Value::as_str);
            let parsed = if object
                .get("calls")
                .and_then(Value::as_array)
                .is_some_and(|a| !a.is_empty())
            {
                object["calls"].clone()
            } else if let Some(encoded) = encoded {
                if encoded.len() > 32 * 1024 {
                    return Err(ParseError::TooLarge);
                }
                serde_json::from_str(encoded).map_err(|_| ParseError::Invalid)?
            } else {
                return Err(ParseError::Invalid);
            };
            let calls = parsed
                .as_array()
                .filter(|a| !a.is_empty() && a.len() <= 8)
                .ok_or(ParseError::Invalid)?;
            let mut out = Vec::with_capacity(calls.len());
            for item in calls {
                out.push(parse_call(item, None, out.len())?);
            }
            Ok(Decision::ToolCall {
                calls: out,
                requires_observation: declared,
            })
        }
        _ => {
            let implicit = if action != "tool" && tool_registry::find(&action).is_some() {
                Some(action.as_str())
            } else {
                None
            };
            if action != "tool" && implicit.is_none() {
                return Err(ParseError::Invalid);
            }
            Ok(Decision::ToolCall {
                calls: vec![parse_call(&value, implicit, 0)?],
                requires_observation: declared,
            })
        }
    }
}
fn parse_call(value: &Value, implicit: Option<&str>, index: usize) -> Result<ToolCall, ParseError> {
    let object = value.as_object().ok_or(ParseError::Invalid)?;
    let id = object
        .get("tool")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|v| !v.is_empty())
        .or(implicit)
        .ok_or(ParseError::Invalid)?;
    let descriptor = tool_registry::find(id).ok_or(ParseError::UnknownTool)?;
    let args = if object.get("args").is_some_and(Value::is_object) {
        object["args"].clone()
    } else {
        let text = object
            .get("argsJson")
            .and_then(Value::as_str)
            .ok_or(ParseError::Invalid)?;
        if text.len() > MAX_ARGS_BYTES {
            return Err(ParseError::TooLarge);
        }
        serde_json::from_str(text).map_err(|_| ParseError::Invalid)?
    };
    let invocation =
        tool_registry::plan(descriptor.tool, &args).map_err(|_| ParseError::InvalidArguments)?;
    let depends_on = match object.get("dependsOn") {
        None => Vec::new(),
        Some(Value::Array(items)) => items
            .iter()
            .map(|v| {
                v.as_u64()
                    .and_then(|n| usize::try_from(n).ok())
                    .filter(|n| *n < index)
                    .ok_or(ParseError::Invalid)
            })
            .collect::<Result<Vec<_>, _>>()?,
        _ => return Err(ParseError::Invalid),
    };
    Ok(ToolCall {
        invocation,
        depends_on,
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BudgetStop {
    IterationLimit,
    Blocked,
    RejectedFinals,
}
pub struct Budget {
    pub productive: usize,
    pub blocked: usize,
    pub consecutive_blocked: usize,
    pub rejected_finals: usize,
    pub max_iterations: usize,
    pub max_blocked: usize,
    pub max_consecutive_blocked: usize,
}
impl Default for Budget {
    fn default() -> Self {
        Self {
            productive: 0,
            blocked: 0,
            consecutive_blocked: 0,
            rejected_finals: 0,
            max_iterations: 24,
            max_blocked: 6,
            max_consecutive_blocked: 4,
        }
    }
}
impl Budget {
    pub fn admit_next(&self) -> Result<(), BudgetStop> {
        if self.blocked >= self.max_blocked
            || self.consecutive_blocked >= self.max_consecutive_blocked
        {
            Err(BudgetStop::Blocked)
        } else if self.productive >= self.max_iterations {
            Err(BudgetStop::IterationLimit)
        } else {
            Ok(())
        }
    }
    pub fn record_productive(&mut self) {
        self.productive = self.productive.saturating_add(1);
        self.consecutive_blocked = 0;
    }
    pub fn record_blocked(&mut self) {
        self.blocked = self.blocked.saturating_add(1);
        self.consecutive_blocked = self.consecutive_blocked.saturating_add(1);
    }
    pub fn reject_final(&mut self) -> Result<(), BudgetStop> {
        self.rejected_finals = self.rejected_finals.saturating_add(1);
        if self.rejected_finals >= 2 {
            Err(BudgetStop::RejectedFinals)
        } else {
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    #[test]
    fn decisions_are_structured_bounded_and_do_not_accept_unknown_tools() {
        let decision = parse(
            r#"```json
{"action":"read-file","args":{"path":"note"}}
```"#,
        )
        .unwrap();
        let Decision::ToolCall { calls, .. } = decision else {
            panic!("wrong decision")
        };
        assert_eq!(calls[0].invocation.descriptor().id, "read-file");
        assert_eq!(calls[0].invocation.arguments()["offset"], 1);
        assert!(matches!(
            parse(r#"prose {"action":"final"}"#),
            Err(ParseError::Invalid)
        ));
        assert!(matches!(
            parse(r#"{"action":"tool","tool":"unknown","args":{}}"#),
            Err(ParseError::UnknownTool)
        ));
        assert!(matches!(
            parse(r#"{"action":"giveup"}"#),
            Err(ParseError::Invalid)
        ));
        assert!(matches!(
            parse(r#"{"action":"final","grounding":"verified","claims":[]}"#),
            Ok(Decision::Final { .. })
        ));
        let batch=json!({"action":"tool_batch","calls":[{"tool":"read-file","args":{"path":"a"}},{"tool":"read-file","args":{"path":"b"},"dependsOn":[0]}]}).to_string();
        let Decision::ToolCall { calls, .. } = parse(&batch).unwrap() else {
            panic!("wrong decision")
        };
        assert_eq!(calls[1].depends_on, [0]);
        let invalid=json!({"action":"tool_batch","calls":[{"tool":"read-file","args":{"path":"a"},"dependsOn":[0]}]}).to_string();
        assert!(matches!(parse(&invalid), Err(ParseError::Invalid)));
    }
    #[test]
    fn production_budget_limits_and_resets_consecutive_blocks() {
        let mut budget = Budget::default();
        for _ in 0..3 {
            budget.record_blocked();
        }
        assert!(budget.admit_next().is_ok());
        budget.record_productive();
        assert_eq!(budget.consecutive_blocked, 0);
        for _ in 0..3 {
            budget.record_blocked();
        }
        assert_eq!(budget.admit_next(), Err(BudgetStop::Blocked));
        let mut finals = Budget::default();
        assert!(finals.reject_final().is_ok());
        assert_eq!(finals.reject_final(), Err(BudgetStop::RejectedFinals));
    }
    #[test]
    fn schedule_prechecks_scope_and_propagates_failed_dependencies() {
        let raw=json!({"action":"tool_batch","calls":[{"tool":"read-file","args":{"path":"a"}},{"tool":"read-file","args":{"path":"b"},"dependsOn":[0]},{"tool":"read-file","args":{"path":"c"},"dependsOn":[1]},{"tool":"read-file","args":{"path":"d"}}]}).to_string();
        let Decision::ToolCall { calls, .. } = parse(&raw).unwrap() else {
            panic!("wrong decision")
        };
        assert!(matches!(
            BatchSchedule::new(&calls, |i| i.arguments()["path"] != "c"),
            Err(ScheduleError::OutOfScope)
        ));
        let mut schedule = BatchSchedule::new(&calls, |_| true).unwrap();
        assert_eq!(schedule.next().unwrap(), Some(0));
        assert!(matches!(schedule.next(), Err(ScheduleError::Busy)));
        schedule.finish(0, false).unwrap();
        assert_eq!(schedule.next().unwrap(), Some(3));
        assert_eq!(schedule.states()[1], CallState::Blocked);
        assert_eq!(schedule.states()[2], CallState::Blocked);
        schedule.finish(3, true).unwrap();
        assert_eq!(schedule.next().unwrap(), None);
        assert!(schedule.call(3).is_some());
    }
    #[test]
    fn repeated_calls_and_failed_cycles_are_detected_without_dispatch() {
        use crate::tool_registry::Tool;
        let a = tool_registry::plan(Tool::ReadFile, &json!({"path":"a"})).unwrap();
        let b = tool_registry::plan(Tool::ReadFile, &json!({"path":"b"})).unwrap();
        let mut guard = RepeatGuard::default();
        guard.record(&a, false, false);
        assert_eq!(guard.check(&a), Some(RepeatReason::Immediate));
        guard.record(&b, false, false);
        guard.record(&a, false, false);
        guard.record(&b, false, false);
        assert_eq!(guard.check(&a), Some(RepeatReason::FailedCycle));
        let mut guard = RepeatGuard::default();
        guard.record(&a, true, true);
        guard.record(&b, true, true);
        assert_eq!(guard.check(&a), Some(RepeatReason::PriorVerified));
        let write =
            tool_registry::plan(Tool::WriteFile, &json!({"path":"new","content":"x"})).unwrap();
        guard.record(&write, true, true);
        assert_eq!(guard.check(&a), None);
    }
}
