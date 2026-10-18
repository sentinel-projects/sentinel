#include <QtTest>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

namespace proposed_reference_only {

enum class ResourceClass : std::uint8_t { OwnedProcess, Artifact, ExternalObservation };

struct ResourceIdentity {
    ResourceClass kind;
    QString key;
    quint64 revision;

    bool operator==(const ResourceIdentity&) const = default;
    bool operator<(const ResourceIdentity& other) const {
        return std::tie(kind, key, revision) < std::tie(other.kind, other.key, other.revision);
    }
};

struct OwnerIdentity {
    QString daemonGeneration;
    QString request;
};

struct Token {
    QString daemonGeneration;
    quint64 serial;
};

class LeaseReferenceModel final {
public:
    explicit LeaseReferenceModel(QString generation) : generation_(std::move(generation)) {}

    void publish(const ResourceIdentity& resource) {
        const std::scoped_lock lock(mutex_);
        resources_.try_emplace(resource);
    }

    std::optional<Token> admit(const ResourceIdentity& resource, const OwnerIdentity& owner,
                               bool authorized, bool ready) {
        const std::scoped_lock lock(mutex_);
        const auto found = resources_.find(resource);
        if (!authorized || !ready || owner.daemonGeneration != generation_ ||
            owner.request.isEmpty() || terminalOwners_.contains(owner.request) ||
            found == resources_.end() || found->second.closed) {
            return std::nullopt;
        }
        for (const auto& [serial, lease] : leases_) {
            if (lease.owner == owner.request && lease.resource == resource) {
                return Token{generation_, serial};
            }
        }
        const auto serial = ++serial_;
        leases_.emplace(serial, Lease{resource, owner.request});
        ++found->second.consumers;
        return Token{generation_, serial};
    }

    bool terminal(const OwnerIdentity& owner) {
        const std::scoped_lock lock(mutex_);
        if (owner.daemonGeneration != generation_ ||
            !terminalOwners_.insert(owner.request).second) {
            return false;
        }
        for (auto lease = leases_.begin(); lease != leases_.end();) {
            if (lease->second.owner == owner.request) {
                --resources_.at(lease->second.resource).consumers;
                lease = leases_.erase(lease);
            } else {
                ++lease;
            }
        }
        return true;
    }

    bool release(const Token& token) {
        const std::scoped_lock lock(mutex_);
        const auto lease = leases_.find(token.serial);
        if (token.daemonGeneration != generation_ || lease == leases_.end()) {
            return false;
        }
        --resources_.at(lease->second.resource).consumers;
        leases_.erase(lease);
        return true;
    }

    bool retire(const ResourceIdentity& resource) {
        const std::scoped_lock lock(mutex_);
        const auto found = resources_.find(resource);
        if (found == resources_.end()) {
            return false;
        }
        found->second.closed = true;
        return found->second.consumers == 0;
    }

    bool eraseArtifact(const ResourceIdentity& resource) {
        const std::scoped_lock lock(mutex_);
        const auto found = resources_.find(resource);
        if (resource.kind != ResourceClass::Artifact || found == resources_.end() ||
            found->second.consumers != 0) {
            return false;
        }
        found->second.closed = true;
        return true;
    }

    int consumers(const ResourceIdentity& resource) const {
        const std::scoped_lock lock(mutex_);
        return resources_.at(resource).consumers;
    }

private:
    struct ResourceState {
        int consumers = 0;
        bool closed = false;
    };
    struct Lease {
        ResourceIdentity resource;
        QString owner;
    };
    QString generation_;
    quint64 serial_ = 0;
    mutable std::mutex mutex_;
    std::map<ResourceIdentity, ResourceState> resources_;
    std::map<quint64, Lease> leases_;
    std::set<QString> terminalOwners_;
};

} // namespace proposed_reference_only

using namespace proposed_reference_only;

class ResourceLeaseReferenceContractTest final : public QObject {
    Q_OBJECT
private slots:
    void twoRunsRetainArtifactUntilBothTerminate() {
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity artifact{ResourceClass::Artifact, "managed-content-a", 1};
        reference.publish(artifact);
        QCOMPARE(reference.consumers(artifact), 0);
        QVERIFY(reference.admit(artifact, {"daemon-a", "agent-1"}, true, true));
        QVERIFY(reference.admit(artifact, {"daemon-a", "chat-2"}, true, true));
        QCOMPARE(reference.consumers(artifact), 2);
        QVERIFY(!reference.eraseArtifact(artifact));
        QVERIFY(reference.terminal({"daemon-a", "agent-1"}));
        QVERIFY(!reference.eraseArtifact(artifact));
        QVERIFY(reference.terminal({"daemon-a", "chat-2"}));
        QVERIFY(reference.eraseArtifact(artifact));
        QVERIFY(!reference.admit(artifact, {"daemon-a", "agent-3"}, true, true));
    }

    void switchRetainsOldResource_data() {
        QTest::addColumn<QString>("replacement");
        QTest::newRow("provider-switch") << QString("other-provider-handle");
        QTest::newRow("endpoint-switch") << QString("other-endpoint-handle");
    }

    void switchRetainsOldResource() {
        QFETCH(QString, replacement);
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity previous{ResourceClass::OwnedProcess, "owned-handle", 1};
        const ResourceIdentity next{ResourceClass::OwnedProcess, replacement, 2};
        reference.publish(previous);
        QVERIFY(reference.admit(previous, {"daemon-a", "agent-1"}, true, true));
        QVERIFY(!reference.retire(previous));
        reference.publish(next);
        QVERIFY(!reference.admit(previous, {"daemon-a", "agent-2"}, true, true));
        QVERIFY(reference.admit(next, {"daemon-a", "agent-2"}, true, true));
        QCOMPARE(reference.consumers(previous), 1);
        QVERIFY(reference.terminal({"daemon-a", "agent-1"}));
        QVERIFY(reference.retire(previous));
        QCOMPARE(reference.consumers(next), 1);
    }

    void terminalAndCancellationReleaseExactlyOnce() {
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity artifact{ResourceClass::Artifact, "artifact", 1};
        const ResourceIdentity process{ResourceClass::OwnedProcess, "owned-handle", 1};
        reference.publish(artifact);
        reference.publish(process);
        const auto token = reference.admit(artifact, {"daemon-a", "agent-1"}, true, true);
        QVERIFY(token);
        QVERIFY(reference.admit(process, {"daemon-a", "agent-1"}, true, true));
        const auto duplicate = reference.admit(artifact, {"daemon-a", "agent-1"}, true, true);
        if (!duplicate || !token) {
            QFAIL("Reference admission must return a token.");
        } else {
            QCOMPARE(duplicate->serial, token->serial);
        }
        QCOMPARE(reference.consumers(artifact), 1);
        QVERIFY(reference.terminal({"daemon-a", "agent-1"}));
        QVERIFY(!reference.terminal({"daemon-a", "agent-1"}));
        if (token) {
            QVERIFY(!reference.release(*token));
        }
        QVERIFY(!reference.admit(artifact, {"daemon-a", "agent-1"}, true, true));
        QCOMPARE(reference.consumers(artifact), 0);
        QCOMPARE(reference.consumers(process), 0);
        const auto next = reference.admit(artifact, {"daemon-a", "agent-2"}, true, true);
        if (!next) {
            QFAIL("A different owner must still be admissible.");
        } else {
            QVERIFY(reference.release(*next));
            QVERIFY(!reference.release(*next));
        }
        QVERIFY(reference.terminal({"daemon-a", "agent-2"}));
        QCOMPARE(reference.consumers(artifact), 0);
    }

    void failedAdmissionHasNoLease_data() {
        QTest::addColumn<bool>("authorized");
        QTest::addColumn<bool>("ready");
        QTest::newRow("startup-failed") << true << false;
        QTest::newRow("provider-not-ready") << true << false;
        QTest::newRow("permission-refused") << false << true;
    }

    void failedAdmissionHasNoLease() {
        QFETCH(bool, authorized);
        QFETCH(bool, ready);
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity resource{ResourceClass::OwnedProcess, "owned-handle", 1};
        reference.publish(resource);
        QVERIFY(!reference.admit(resource, {"daemon-a", "agent-1"}, authorized, ready));
        QCOMPARE(reference.consumers(resource), 0);
    }

    void restartRejectsOldGenerationEvenWhenSerialIsReused() {
        const ResourceIdentity artifact{ResourceClass::Artifact, "artifact", 1};
        LeaseReferenceModel previous("daemon-a");
        previous.publish(artifact);
        const auto oldToken = previous.admit(artifact, {"daemon-a", "run"}, true, true);
        QVERIFY(oldToken);
        LeaseReferenceModel restarted("daemon-b");
        restarted.publish(artifact);
        const auto newToken = restarted.admit(artifact, {"daemon-b", "run"}, true, true);
        QVERIFY(newToken);
        if (!oldToken || !newToken) {
            QFAIL("Both daemon generations must admit their own request.");
        } else {
            QCOMPARE(oldToken->serial, newToken->serial);
            QVERIFY(!restarted.release(*oldToken));
        }
        QVERIFY(!restarted.terminal({"daemon-a", "run"}));
        QVERIFY(!restarted.admit(artifact, {"daemon-a", "run"}, true, true));
        QCOMPARE(restarted.consumers(artifact), 1);
    }

    void staleIdentityCannotReopenRetiredResource() {
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity oldArtifact{ResourceClass::Artifact, "same-path", 1};
        const ResourceIdentity replacement{ResourceClass::Artifact, "same-path", 2};
        reference.publish(oldArtifact);
        QVERIFY(reference.eraseArtifact(oldArtifact));
        reference.publish(oldArtifact);
        reference.publish(replacement);
        QVERIFY(!reference.admit(oldArtifact, {"daemon-a", "run"}, true, true));
        QVERIFY(reference.admit(replacement, {"daemon-a", "run"}, true, true));
    }

    void externalObservationsHaveNoDestructiveAuthority_data() {
        QTest::addColumn<QString>("endpoint");
        QTest::newRow("shared-ollama") << QString("ollama-loopback");
        QTest::newRow("shared-lm-studio") << QString("lm-studio-loopback");
        QTest::newRow("remote") << QString("remote-provider");
    }

    void externalObservationsHaveNoDestructiveAuthority() {
        QFETCH(QString, endpoint);
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity observation{ResourceClass::ExternalObservation, endpoint, 1};
        reference.publish(observation);
        QVERIFY(reference.admit(observation, {"daemon-a", "run"}, true, true));
        QVERIFY(!reference.eraseArtifact(observation));
        QVERIFY(reference.terminal({"daemon-a", "run"}));
        QVERIFY(reference.retire(observation));
        QVERIFY(!reference.eraseArtifact(observation));
    }

    void concurrentAcquisitionAndCancellationRemainBalanced() {
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity artifact{ResourceClass::Artifact, "artifact", 1};
        reference.publish(artifact);
        std::atomic<int> admitted = 0;
        std::vector<std::thread> workers;
        workers.reserve(8);
        for (int index = 0; index < 8; ++index) {
            workers.emplace_back([&, index] {
                if (reference.admit(artifact, {"daemon-a", QString::number(index)}, true, true)) {
                    ++admitted;
                }
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        QCOMPARE(admitted.load(), 8);
        QCOMPARE(reference.consumers(artifact), 8);
        workers.clear();
        for (int index = 0; index < 8; ++index) {
            workers.emplace_back([&, index] {
                reference.terminal({"daemon-a", QString::number(index)});
                reference.terminal({"daemon-a", QString::number(index)});
            });
        }
        for (auto& worker : workers) {
            worker.join();
        }
        QCOMPARE(reference.consumers(artifact), 0);
        QVERIFY(reference.eraseArtifact(artifact));
    }

    void deleteAndAdmissionSerializeWithoutCheckThenActGap() {
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity artifact{ResourceClass::Artifact, "artifact", 1};
        reference.publish(artifact);
        bool admitted = false;
        bool removed = false;
        std::thread admission([&] {
            admitted = reference.admit(artifact, {"daemon-a", "run"}, true, true).has_value();
        });
        std::thread deletion([&] { removed = reference.eraseArtifact(artifact); });
        admission.join();
        deletion.join();
        QVERIFY(admitted != removed);
        QCOMPARE(reference.consumers(artifact), admitted ? 1 : 0);
    }

    void cancellationRacingAdmissionCannotLeaveTerminalOwnerLeased() {
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity artifact{ResourceClass::Artifact, "artifact", 1};
        reference.publish(artifact);
        std::thread admission([&] { reference.admit(artifact, {"daemon-a", "run"}, true, true); });
        std::thread cancellation([&] { reference.terminal({"daemon-a", "run"}); });
        admission.join();
        cancellation.join();
        QCOMPARE(reference.consumers(artifact), 0);
        QVERIFY(!reference.admit(artifact, {"daemon-a", "run"}, true, true));
    }

    void cancellationDuringReleaseCannotDecrementTwice() {
        LeaseReferenceModel reference("daemon-a");
        const ResourceIdentity artifact{ResourceClass::Artifact, "artifact", 1};
        reference.publish(artifact);
        const auto token = reference.admit(artifact, {"daemon-a", "run"}, true, true);
        if (!token) {
            QFAIL("Reference admission must return a token.");
        } else {
            std::thread release([&] { reference.release(*token); });
            std::thread cancellation([&] { reference.terminal({"daemon-a", "run"}); });
            release.join();
            cancellation.join();
            QVERIFY(!reference.release(*token));
        }
        QCOMPARE(reference.consumers(artifact), 0);
        QVERIFY(!reference.terminal({"daemon-a", "run"}));
    }
};

QTEST_GUILESS_MAIN(ResourceLeaseReferenceContractTest)
#include "test_resource_lease_reference_contract.moc"
