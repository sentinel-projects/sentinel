#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/memory/InMemorySettingsStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/provider/ProviderRegistry.h"
#include "sentinel/core/runtime/LocalInference.h"
#include <QtTest>
#include <stdexcept>

using namespace sentinel::core;

class ProviderRegistryTest final : public QObject {
    Q_OBJECT
private slots:
    void descriptorsMatchHistoricalOrderAndProtocols() {
        const auto& registry = ProviderRegistry::builtins();
        const QStringList expected{"ollama",
                                   "lm-studio",
                                   "openai-compatible-local",
                                   "llama-cpp-server",
                                   "cloud-api",
                                   "openai",
                                   "openai-compatible",
                                   "claude",
                                   "gemini",
                                   "deepseek",
                                   "groq",
                                   "mistral"};
        QStringList actual;
        for (const auto& descriptor : registry.descriptors()) {
            actual.append(descriptor.id);
            QVERIFY(!descriptor.displayName.isEmpty());
            QCOMPARE(registry.find(descriptor.id), &descriptor);
            const auto protocol =
                descriptor.id == "ollama"      ? ProviderProtocol::Ollama
                : descriptor.id == "claude"    ? ProviderProtocol::AnthropicMessages
                : descriptor.id == "gemini"    ? ProviderProtocol::GeminiGenerateContent
                : descriptor.id == "cloud-api" ? ProviderProtocol::SelectedCloud
                                               : ProviderProtocol::OpenAiCompatible;
            QCOMPARE(descriptor.protocol, protocol);
            QCOMPARE(descriptor.adapter, descriptor.id == "ollama"
                                             ? ProviderAdapter::Ollama
                                             : ProviderAdapter::SelectedEndpoint);
            QCOMPARE(descriptor.declaredCapabilities.provenance.streaming,
                     ModelMetadataSource::ProviderDefault);
            QCOMPARE(descriptor.declaredCapabilities.provenance.visionInput,
                     ModelMetadataSource::Unknown);
        }
        QCOMPARE(actual, expected);
        QVERIFY(!registry.find("unknown"));
        QVERIFY(!registry.find("OPENAI"));
        QCOMPARE(registry.discoveryStrategy("unknown"), ProviderDiscovery::None);
    }
    void rejectsDuplicateDescriptorsButPreservesFactoryOverrides() {
        auto descriptors = ProviderRegistry::builtins().descriptors();
        descriptors.append(descriptors.first());
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, ProviderRegistry{descriptors});
        descriptors = ProviderRegistry::builtins().descriptors();
        descriptors.first().id = " OLLAMA ";
        QVERIFY_THROWS_EXCEPTION(std::invalid_argument, ProviderRegistry{descriptors});
        ModelService service;
        ModelCapabilities replacement;
        replacement.streaming = CapabilitySupport::Unsupported;
        service.registerProvider(
            " OPENAI ", [](const ModelBinding&) { return std::shared_ptr<IChatProvider>{}; },
            replacement);
        QCOMPARE(service.capabilities("openai", "fixture").streaming,
                 CapabilitySupport::Unsupported);
        QCOMPARE(service.knownProviderIds().size(), 12);
    }
    void preservesDiscoverySelectionAndFacadeAsymmetry() {
        const auto& registry = ProviderRegistry::builtins();
        for (const auto& pair : QList<QPair<QString, ProviderDiscovery>>{
                 {"ollama", ProviderDiscovery::OllamaTags},
                 {"lm-studio", ProviderDiscovery::OpenAiLocalModels},
                 {"openai-compatible-local", ProviderDiscovery::OpenAiLocalModels},
                 {"llama-cpp-server", ProviderDiscovery::LlamaCppModels},
                 {"openai", ProviderDiscovery::OpenAiCloudModels},
                 {"openai-compatible", ProviderDiscovery::None},
                 {"claude", ProviderDiscovery::AnthropicModels},
                 {"gemini", ProviderDiscovery::GeminiModels},
                 {"deepseek", ProviderDiscovery::OpenAiCloudModels},
                 {"groq", ProviderDiscovery::OpenAiCloudModels},
                 {"mistral", ProviderDiscovery::OpenAiCloudModels}}) {
            QCOMPARE(registry.discoveryStrategy(pair.first), pair.second);
            if (registry.find(pair.first)->kind == ProviderKind::Cloud) {
                QCOMPARE(registry.discoveryStrategy("cloud-api", pair.first), pair.second);
            }
        }
        for (const auto& selected : QStringList{"unknown", "ollama", "cloud-api", ""}) {
            QCOMPARE(registry.endpointDescriptor("cloud-api", selected)->id, QString("openai"));
            QCOMPARE(registry.discoveryStrategy("cloud-api", selected), ProviderDiscovery::None);
        }
        QVERIFY(!registry.endpointDescriptor("unknown", "openai"));
    }
    void customCloudEndpointAndCredentialChangesInvalidateDiscovery() {
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        settings.setOpenAiApiKey("fixture-first");
        ModelService service(&settings);
        const auto oldConfig = service.providerConfig({"cloud-api", "fixture"});
        const auto oldSequence = service.beginProviderHealthObservation();
        service.acceptProviderDiscovery(
            "cloud-api", {},
            {false, ChatProviderErrorCategory::AuthenticationRequired, 401, "fixture"},
            oldSequence);
        service.reportProviderDiscovery("cloud-api", oldSequence, false,
                                        ChatProviderErrorCategory::AuthenticationRequired);
        settings.setCloudApiEndpoint("https://gateway.example/prefix/v1/");
        const auto custom = service.providerConfig({"cloud-api", "fixture"});
        QCOMPARE(custom.endpoint, QUrl("https://gateway.example/prefix/v1/"));
        QCOMPARE(custom.apiKey, QString("fixture-first"));
        QCOMPARE(service.providerConfig({"openai", "fixture"}).endpoint,
                 QUrl("https://api.openai.com"));
        QCOMPARE(oldConfig.endpoint, QUrl("https://api.openai.com"));
        // A response from the previous address must not restore its catalog/health.
        service.acceptProviderDiscovery("cloud-api", {{"old-model", {}, 0}},
                                        {true, ChatProviderErrorCategory::None, 200, "fixture"},
                                        oldSequence);
        service.reportProviderDiscovery("cloud-api", oldSequence, true,
                                        ChatProviderErrorCategory::None);
        QVERIFY(service.providerDiscoveredModels("cloud-api").isEmpty());
        QCOMPARE(service.providerHealth("cloud-api"), ProviderHealth::Unknown);
        const auto newSequence = service.beginProviderHealthObservation();
        service.acceptProviderDiscovery("cloud-api", {{"new-model", {}, 0}},
                                        {true, ChatProviderErrorCategory::None, 200, "fixture"},
                                        newSequence);
        QCOMPARE(service.providerStatus("cloud-api").catalog, ProviderCatalogState::Available);
        settings.setOpenAiApiKey("fixture-replacement");
        QCOMPARE(service.providerConfig({"cloud-api", "fixture"}).apiKey,
                 QString("fixture-replacement"));
        QCOMPARE(custom.apiKey, QString("fixture-first"));
        QVERIFY(service.providerDiscoveredModels("cloud-api").isEmpty());
        settings.setCloudApiEndpoint("http://remote.example/v1");
        QVERIFY(!service.providerConfig({"cloud-api", "fixture"}).isAllowedEndpoint());
        settings.setSelectedCloudProvider("gemini");
        QCOMPARE(service.providerConfig({"cloud-api", "fixture"}).endpoint,
                 QUrl("https://generativelanguage.googleapis.com"));
    }
    void accessDeniedDoesNotClaimTheKeyIsMissing() {
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        settings.setOpenAiApiKey("fixture-key");
        ModelService service(&settings);
        service.acceptProviderDiscovery(
            "cloud-api", {},
            {false, ChatProviderErrorCategory::AuthenticationRequired, 403, "fixture"},
            service.beginProviderHealthObservation());
        const auto detail = service.providerStatusSnapshot("cloud-api").safeDetail;
        QVERIFY(detail.contains("HTTP 403"));
        QVERIFY(detail.contains("terms"));
        QVERIFY(!detail.contains("fixture-key"));
        QCOMPARE(service.providerStatus("cloud-api").safeDetail, detail);
    }
    void characterizesExistingRegistration() {
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        ModelService service(&settings);
        QStringList ids{"ollama",
                        "lm-studio",
                        "openai-compatible-local",
                        "llama-cpp-server",
                        "cloud-api",
                        "openai",
                        "openai-compatible",
                        "claude",
                        "gemini",
                        "deepseek",
                        "groq",
                        "mistral"};
        ids.sort();
        QCOMPARE(service.knownProviderIds(), ids);
        for (const auto& id : ids) {
            const auto caps = service.capabilities(id, "fixture");
            QCOMPARE(caps.streaming, CapabilitySupport::Supported);
            QCOMPARE(caps.provenance.streaming, ModelMetadataSource::ProviderDefault);
            QCOMPARE(caps.nativeToolCalling, id == "claude" || id == "gemini"
                                                 ? CapabilitySupport::Supported
                                                 : CapabilitySupport::Unsupported);
            QCOMPARE(caps.structuredOutput, id == "claude" || id == "gemini"
                                                ? CapabilitySupport::Unknown
                                                : CapabilitySupport::Unsupported);
            QCOMPARE(caps.visionInput, CapabilitySupport::Unknown);
            QCOMPARE(caps.provenance.visionInput, ModelMetadataSource::Unknown);
        }
        QVERIFY(!service.isKnownProvider("unknown"));
        QCOMPARE(service.resolve("unknown", "fixture").error, ModelBindingError::ProviderNotFound);
        service.setSelectedModel({" CLAUDE ", " fixture "});
        QCOMPARE(service.selectedModel(), (ModelSelection{"claude", "fixture"}));
        QCOMPARE(settings.selectedRuntimeProvider(), QString("claude"));
        service.setSelectedProviderId("unknown");
        QCOMPARE(service.selectedModel().providerId, QString("ollama"));
    }
    void characterizesEndpointsAndFacade() {
        AppSettings settings(std::make_unique<InMemorySettingsStore>(),
                             inMemoryTestCredentialStore());
        ModelService service(&settings);
        settings.setOpenAiApiKey("fixture-openai");
        settings.setClaudeApiKey("fixture-claude");
        settings.setGeminiApiKey("fixture-gemini");
        settings.setDeepseekApiKey("fixture-deepseek");
        settings.setGroqApiKey("fixture-groq");
        settings.setMistralApiKey("fixture-mistral");
        service.setLmStudioEndpoint("http://127.0.0.1:2234");
        service.setLlamaCppEndpoint("http://127.0.0.1:9080");
        for (const auto& pair :
             QList<QPair<QString, QString>>{{"lm-studio", "http://127.0.0.1:2234"},
                                            {"llama-cpp-server", "http://127.0.0.1:9080"},
                                            {"openai-compatible-local", "http://127.0.0.1:8000"},
                                            {"openai", "https://api.openai.com"},
                                            {"openai-compatible", "https://api.openai.com"},
                                            {"claude", "https://api.anthropic.com"},
                                            {"gemini", "https://generativelanguage.googleapis.com"},
                                            {"deepseek", "https://api.deepseek.com"},
                                            {"groq", "https://api.groq.com/openai"},
                                            {"mistral", "https://api.mistral.ai"},
                                            {"unknown", "http://127.0.0.1:2234"},
                                            {"OPENAI", "http://127.0.0.1:2234"},
                                            {"ollama", "http://127.0.0.1:2234"}}) {
            QCOMPARE(service.providerConfig({pair.first, "fixture"}).endpoint, QUrl(pair.second));
        }
        for (const auto& id :
             QStringList{"openai", "claude", "gemini", "deepseek", "groq", "mistral"}) {
            settings.setSelectedCloudProvider(id);
            const auto direct = service.providerConfig({id, "fixture"});
            const auto facade = service.providerConfig({"cloud-api", "fixture"});
            QCOMPARE(direct.apiKey, QString("fixture-") + id);
            QCOMPARE(facade.endpoint, direct.endpoint);
            QCOMPARE(facade.apiKey, direct.apiKey);
        }
    }
};
QTEST_GUILESS_MAIN(ProviderRegistryTest)
#include "test_provider_registry.moc"
