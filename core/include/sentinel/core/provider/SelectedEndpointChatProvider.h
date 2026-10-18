#pragma once

#include <QJsonObject>
#include <memory>

namespace sentinel::core {

class IChatProvider;
struct ModelBinding;
struct LMStudioConfig;
struct ToolDescriptor;

std::shared_ptr<IChatProvider>
makeSelectedEndpointChatProvider(ModelBinding binding, LMStudioConfig config, int timeoutMs);
QJsonObject geminiFunctionDeclaration(const ToolDescriptor& tool);

} // namespace sentinel::core
