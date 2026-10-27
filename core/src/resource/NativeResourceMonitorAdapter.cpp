// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/resource/IResourceMonitorAdapter.h"

#include <QDir>
#include <QFile>
#include <QThread>

#if defined(Q_OS_MACOS)
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/task_info.h>
#include <sys/sysctl.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace sentinel::core {
namespace {

using sentinel::core::HostResourceSnapshot;
using sentinel::core::ResourceCapabilityReport;
using sentinel::core::ResourceMetricAvailability;
using sentinel::core::ResourceSampleTimestamp;
using sentinel::core::ResourceSignal;
using sentinel::core::ResourceSignalSupport;

ResourceCapabilityReport buildCapabilities(const QString& adapterId,
                                           const QList<ResourceSignalSupport>& entries) {
    ResourceCapabilityReport report;
    report.adapterId = adapterId;
    report.entries = entries;
    return report;
}

#if defined(Q_OS_MACOS)

constexpr const char* kAdapterId = "macos.native";

HostResourceSnapshot sampleMacOs(const ResourceSampleTimestamp& capturedAt) {
    HostResourceSnapshot snapshot;
    snapshot.adapterId = QString::fromLatin1(kAdapterId);
    snapshot.capturedAt = capturedAt;
    snapshot.hostLogicalCpuCount = QThread::idealThreadCount();

    const auto origin = sentinel::core::ResourceMeasurementOrigin::SentinelProcess;

    // --- process ---
    mach_task_basic_info_data_t basic{};
    mach_msg_type_number_t basicCount = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&basic),
                  &basicCount) == KERN_SUCCESS) {
        const auto cpuSeconds = static_cast<double>(basic.user_time.seconds) +
                                static_cast<double>(basic.user_time.microseconds) / 1'000'000.0 +
                                static_cast<double>(basic.system_time.seconds) +
                                static_cast<double>(basic.system_time.microseconds) / 1'000'000.0;
        snapshot.process.cpuSecondsTotal = sentinel::core::measuredMetric<double>(
            cpuSeconds, capturedAt, origin, sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("macos.mach_task_basic_info"));
        snapshot.process.residentBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(basic.resident_size), capturedAt, origin,
            sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("macos.mach_task_basic_info"));
        snapshot.process.virtualBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(basic.virtual_size), capturedAt, origin,
            sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("macos.mach_task_basic_info"));
    } else {
        snapshot.process.residentBytes = sentinel::core::unknownMetric<qint64>(
            origin, QStringLiteral("macos.mach_task_basic_info"),
            QStringLiteral("task_info MACH_TASK_BASIC_INFO failed."));
    }

    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t threadCount = 0;
    if (task_threads(mach_task_self(), &threads, &threadCount) == KERN_SUCCESS) {
        snapshot.process.threadCount =
            sentinel::core::measuredMetric<int>(static_cast<int>(threadCount), capturedAt, origin,
                                                sentinel::core::ResourceReliability::Authoritative,
                                                QStringLiteral("macos.task_threads"));
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                      threadCount * sizeof(thread_act_t));
    }

    // --- host memory ---
    uint64_t totalRam = 0;
    size_t totalRamSize = sizeof(totalRam);
    if (sysctlbyname("hw.memsize", &totalRam, &totalRamSize, nullptr, 0) == 0) {
        snapshot.hostMemory.totalBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(totalRam), capturedAt,
            sentinel::core::ResourceMeasurementOrigin::HostKernel,
            sentinel::core::ResourceReliability::Authoritative, QStringLiteral("macos.hw.memsize"));
    }

    mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t vmCount = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    if (host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &vmCount) ==
            KERN_SUCCESS &&
        host_page_size(host, &pageSize) == KERN_SUCCESS && pageSize > 0) {
        const uint64_t availablePages =
            static_cast<uint64_t>(vm.free_count) + static_cast<uint64_t>(vm.inactive_count) +
            static_cast<uint64_t>(vm.speculative_count) + static_cast<uint64_t>(vm.purgeable_count);
        snapshot.hostMemory.availableBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(availablePages * pageSize), capturedAt,
            sentinel::core::ResourceMeasurementOrigin::HostKernel,
            sentinel::core::ResourceReliability::Approximate,
            QStringLiteral("macos.host_statistics64"));
    }
    mach_port_deallocate(mach_task_self(), host);

    int pressureLevel = 0;
    size_t pressureSize = sizeof(pressureLevel);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressureLevel, &pressureSize, nullptr,
                     0) == 0) {
        // Native levels: 1 = normal, 2 = warning, 4 = critical. The mapping is
        // a documented normalization, so it is Approximate rather than exact.
        std::optional<double> normalized;
        if (pressureLevel == 1) {
            normalized = 0.0;
        } else if (pressureLevel == 2) {
            normalized = 0.5;
        } else if (pressureLevel == 4) {
            normalized = 1.0;
        }
        if (normalized) {
            snapshot.hostMemory.pressureLevel = sentinel::core::measuredMetric<double>(
                *normalized, capturedAt, sentinel::core::ResourceMeasurementOrigin::HostKernel,
                sentinel::core::ResourceReliability::Approximate,
                QStringLiteral("macos.kern.memorystatus_vm_pressure_level"));
        } else {
            snapshot.hostMemory.pressureLevel = sentinel::core::unknownMetric<double>(
                sentinel::core::ResourceMeasurementOrigin::HostKernel,
                QStringLiteral("macos.kern.memorystatus_vm_pressure_level"),
                QStringLiteral("Unrecognized native pressure level."));
        }
    }

    // --- power (IOKit power sources; no privileged helper) ---
    CFTypeRef powerInfo = IOPSCopyPowerSourcesInfo();
    if (powerInfo) {
        const auto platformOrigin = sentinel::core::ResourceMeasurementOrigin::PlatformService;
        CFStringRef sourceType = IOPSGetProvidingPowerSourceType(powerInfo);
        if (sourceType) {
            sentinel::core::ResourcePowerSource source =
                sentinel::core::ResourcePowerSource::Unknown;
            if (CFStringCompare(sourceType, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo) {
                source = sentinel::core::ResourcePowerSource::Ac;
            } else if (CFStringCompare(sourceType, CFSTR(kIOPSBatteryPowerValue), 0) ==
                       kCFCompareEqualTo) {
                source = sentinel::core::ResourcePowerSource::Battery;
            }
            snapshot.power.powerSource =
                sentinel::core::measuredMetric(source, capturedAt, platformOrigin,
                                               sentinel::core::ResourceReliability::Authoritative,
                                               QStringLiteral("macos.IOPSCopyPowerSourcesInfo"));
        }

        CFArrayRef sources = IOPSCopyPowerSourcesList(powerInfo);
        bool batteryPresent = false;
        bool chargeKnown = false;
        double chargeFraction = 0.0;
        if (sources) {
            const CFIndex count = CFArrayGetCount(sources);
            for (CFIndex index = 0; index < count; ++index) {
                CFDictionaryRef description = IOPSGetPowerSourceDescription(
                    powerInfo, CFArrayGetValueAtIndex(sources, index));
                if (!description) {
                    continue;
                }
                const void* typeValue = CFDictionaryGetValue(description, CFSTR(kIOPSTypeKey));
                const void* presentValue =
                    CFDictionaryGetValue(description, CFSTR(kIOPSIsPresentKey));
                const bool isInternalBattery =
                    typeValue && CFGetTypeID(typeValue) == CFStringGetTypeID() &&
                    CFStringCompare(static_cast<CFStringRef>(typeValue),
                                    CFSTR(kIOPSInternalBatteryType), 0) == kCFCompareEqualTo;
                const bool present =
                    !presentValue || CFBooleanGetValue(static_cast<CFBooleanRef>(presentValue));
                if (!isInternalBattery || !present) {
                    continue;
                }
                batteryPresent = true;
                const void* currentValue =
                    CFDictionaryGetValue(description, CFSTR(kIOPSCurrentCapacityKey));
                const void* maxValue =
                    CFDictionaryGetValue(description, CFSTR(kIOPSMaxCapacityKey));
                int current = 0;
                int max = 0;
                if (currentValue && maxValue && CFNumberGetTypeID() == CFGetTypeID(currentValue) &&
                    CFGetTypeID(maxValue) == CFNumberGetTypeID() &&
                    CFNumberGetValue(static_cast<CFNumberRef>(currentValue), kCFNumberIntType,
                                     &current) &&
                    CFNumberGetValue(static_cast<CFNumberRef>(maxValue), kCFNumberIntType, &max) &&
                    max > 0) {
                    chargeKnown = true;
                    chargeFraction = static_cast<double>(current) / static_cast<double>(max);
                }
            }
            CFRelease(sources);
        }
        snapshot.power.batteryPresent =
            sentinel::core::measuredMetric(batteryPresent, capturedAt, platformOrigin,
                                           sentinel::core::ResourceReliability::Authoritative,
                                           QStringLiteral("macos.IOPSCopyPowerSourcesInfo"));
        if (batteryPresent && chargeKnown) {
            snapshot.power.batteryChargeFraction =
                sentinel::core::measuredMetric(chargeFraction, capturedAt, platformOrigin,
                                               sentinel::core::ResourceReliability::Authoritative,
                                               QStringLiteral("macos.IOPSCopyPowerSourcesInfo"));
        } else if (!batteryPresent) {
            snapshot.power.batteryChargeFraction = sentinel::core::unavailableMetric<double>(
                QStringLiteral("macos.IOPSCopyPowerSourcesInfo"),
                QStringLiteral("No internal battery present."));
        }
        CFRelease(powerInfo);
    } else {
        snapshot.power.batteryPresent = sentinel::core::unknownMetric<bool>(
            sentinel::core::ResourceMeasurementOrigin::PlatformService,
            QStringLiteral("macos.IOPSCopyPowerSourcesInfo"),
            QStringLiteral("IOPSCopyPowerSourcesInfo returned null."));
    }

    // --- thermal ---
    uint32_t thermalLevel = 0;
    if (IOPMGetThermalWarningLevel(&thermalLevel) == kIOReturnSuccess) {
        QString level;
        if (thermalLevel == kIOPMThermalWarningLevelNormal) {
            level = QStringLiteral("nominal");
        } else if (thermalLevel == kIOPMThermalWarningLevelDanger) {
            level = QStringLiteral("serious");
        } else if (thermalLevel == kIOPMThermalWarningLevelCrisis) {
            level = QStringLiteral("critical");
        }
        if (!level.isEmpty()) {
            snapshot.thermal.level = sentinel::core::measuredMetric(
                level, capturedAt, sentinel::core::ResourceMeasurementOrigin::PlatformService,
                sentinel::core::ResourceReliability::Authoritative,
                QStringLiteral("macos.IOPMGetThermalWarningLevel"));
        }
    } else {
        snapshot.thermal.level = sentinel::core::unavailableMetric<QString>(
            QStringLiteral("macos.IOPMGetThermalWarningLevel"),
            QStringLiteral("Thermal warning level not published by this host."));
    }

    // GPU utilization is not independently measurable without private/GPU APIs;
    // it stays Unsupported and VRAM is never inferred from unified memory.
    snapshot.thermal.temperatureCelsius = sentinel::core::unsupportedMetric<double>(
        QStringLiteral("macos.thermal.temperature"),
        QStringLiteral("No public Celsius sensor API on this platform."));
    return snapshot;
}

ResourceCapabilityReport macOsCapabilities() {
    return buildCapabilities(
        QString::fromLatin1(kAdapterId),
        {{ResourceSignal::ProcessCpu, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessResidentMemory, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessVirtualMemory, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessThreadCount, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryTotal, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryAvailable, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryPressure, ResourceMetricAvailability::Measured},
         {ResourceSignal::BatteryPresence, ResourceMetricAvailability::Measured},
         {ResourceSignal::PowerSource, ResourceMetricAvailability::Measured},
         {ResourceSignal::BatteryCharge, ResourceMetricAvailability::Measured},
         {ResourceSignal::ThermalLevel, ResourceMetricAvailability::Measured},
         {ResourceSignal::ThermalTemperature, ResourceMetricAvailability::Unsupported},
         {ResourceSignal::GpuMemory, ResourceMetricAvailability::Unsupported},
         {ResourceSignal::GpuUtilization, ResourceMetricAvailability::Unsupported}});
}

#elif defined(Q_OS_LINUX)

constexpr const char* kAdapterId = "linux.proc";

qint64 readKeyValueBytes(const QString& path, const QString& key) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    while (!file.atEnd()) {
        const auto line = file.readLine();
        if (!line.startsWith(key.toLatin1()))
            continue;
        const auto text = QString::fromLatin1(line).section(QLatin1Char(':'), 1).trimmed();
        const auto fields = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.isEmpty())
            return -1;
        bool ok = false;
        const qint64 kb = fields.first().toLongLong(&ok);
        return ok ? kb * 1024 : -1;
    }
    return -1;
}

HostResourceSnapshot sampleLinux(const ResourceSampleTimestamp& capturedAt) {
    HostResourceSnapshot snapshot;
    snapshot.adapterId = QString::fromLatin1(kAdapterId);
    snapshot.capturedAt = capturedAt;
    snapshot.hostLogicalCpuCount = QThread::idealThreadCount();
    const auto processOrigin = sentinel::core::ResourceMeasurementOrigin::SentinelProcess;
    const auto hostOrigin = sentinel::core::ResourceMeasurementOrigin::HostKernel;

    // --- process stat: utime+stime in clock ticks ---
    QFile statFile(QStringLiteral("/proc/self/stat"));
    if (statFile.open(QIODevice::ReadOnly)) {
        const auto text = QString::fromLatin1(statFile.readAll());
        const int close = text.lastIndexOf(QLatin1Char(')'));
        if (close >= 0) {
            const auto fields = text.mid(close + 2).split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const long clockTicks = sysconf(_SC_CLK_TCK);
            if (clockTicks > 0 && fields.size() > 12) {
                bool okUser = false;
                bool okSystem = false;
                const double userSeconds = fields.at(11).toDouble(&okUser) / clockTicks;
                const double systemSeconds = fields.at(12).toDouble(&okSystem) / clockTicks;
                if (okUser && okSystem) {
                    snapshot.process.cpuSecondsTotal = sentinel::core::measuredMetric<double>(
                        userSeconds + systemSeconds, capturedAt, processOrigin,
                        sentinel::core::ResourceReliability::Authoritative,
                        QStringLiteral("linux.proc.self.stat"));
                }
            }
        }
    }

    const qint64 rss =
        readKeyValueBytes(QStringLiteral("/proc/self/status"), QStringLiteral("VmRSS"));
    if (rss >= 0) {
        snapshot.process.residentBytes = sentinel::core::measuredMetric<qint64>(
            rss, capturedAt, processOrigin, sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("linux.proc.self.status"));
    }
    const qint64 vmSize =
        readKeyValueBytes(QStringLiteral("/proc/self/status"), QStringLiteral("VmSize"));
    if (vmSize >= 0) {
        snapshot.process.virtualBytes = sentinel::core::measuredMetric<qint64>(
            vmSize, capturedAt, processOrigin, sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("linux.proc.self.status"));
    }

    QFile statusFile(QStringLiteral("/proc/self/status"));
    if (statusFile.open(QIODevice::ReadOnly)) {
        while (!statusFile.atEnd()) {
            const auto line = statusFile.readLine();
            if (!line.startsWith("Threads:"))
                continue;
            bool ok = false;
            const int threads =
                QString::fromLatin1(line).section(QLatin1Char(':'), 1).trimmed().toInt(&ok);
            if (ok) {
                snapshot.process.threadCount = sentinel::core::measuredMetric<int>(
                    threads, capturedAt, processOrigin,
                    sentinel::core::ResourceReliability::Authoritative,
                    QStringLiteral("linux.proc.self.status"));
            }
            break;
        }
    }

    // --- host memory ---
    const qint64 total =
        readKeyValueBytes(QStringLiteral("/proc/meminfo"), QStringLiteral("MemTotal"));
    if (total >= 0) {
        snapshot.hostMemory.totalBytes = sentinel::core::measuredMetric<qint64>(
            total, capturedAt, hostOrigin, sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("linux.proc.meminfo"));
    }
    const qint64 available =
        readKeyValueBytes(QStringLiteral("/proc/meminfo"), QStringLiteral("MemAvailable"));
    if (available >= 0) {
        snapshot.hostMemory.availableBytes = sentinel::core::measuredMetric<qint64>(
            available, capturedAt, hostOrigin, sentinel::core::ResourceReliability::Approximate,
            QStringLiteral("linux.proc.meminfo"));
    } else {
        snapshot.hostMemory.availableBytes = sentinel::core::unavailableMetric<qint64>(
            QStringLiteral("linux.proc.meminfo"),
            QStringLiteral("MemAvailable absent (pre-3.14 kernel)."));
    }

    // --- PSI memory pressure ---
    QFile psi(QStringLiteral("/proc/pressure/memory"));
    if (psi.open(QIODevice::ReadOnly)) {
        const auto firstLine = QString::fromLatin1(psi.readLine());
        const auto token = firstLine.section(QLatin1Char(' '), 1, 1); // "avg10=0.00"
        bool ok = false;
        const double avg10 = token.section(QLatin1Char('='), 1, 1).toDouble(&ok);
        if (ok) {
            snapshot.hostMemory.pressureLevel = sentinel::core::measuredMetric<double>(
                qBound(0.0, avg10 / 100.0, 1.0), capturedAt, hostOrigin,
                sentinel::core::ResourceReliability::Approximate,
                QStringLiteral("linux.proc.pressure.memory"));
        }
    } else {
        snapshot.hostMemory.pressureLevel = sentinel::core::unavailableMetric<double>(
            QStringLiteral("linux.proc.pressure.memory"),
            QStringLiteral("PSI memory pressure not exposed by this kernel."));
    }

    // --- power supply ---
    const QDir powerSupplies(QStringLiteral("/sys/class/power_supply"));
    const auto entries = powerSupplies.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (entries.isEmpty()) {
        snapshot.power.batteryPresent = sentinel::core::unsupportedMetric<bool>(
            QStringLiteral("linux.sysfs.power_supply"),
            QStringLiteral("No /sys/class/power_supply entries."));
    } else {
        bool batteryPresent = false;
        bool acOnline = false;
        bool acKnown = false;
        bool chargeKnown = false;
        double charge = 0.0;
        for (const auto& entry : entries) {
            const QString base = powerSupplies.filePath(entry);
            QFile typeFile(base + QStringLiteral("/type"));
            if (!typeFile.open(QIODevice::ReadOnly))
                continue;
            const auto type = QString::fromLatin1(typeFile.readAll()).trimmed();
            if (type == QStringLiteral("Battery")) {
                QFile presentFile(base + QStringLiteral("/present"));
                bool present = true;
                if (presentFile.open(QIODevice::ReadOnly))
                    present = presentFile.readAll().trimmed().toInt() != 0;
                if (!present)
                    continue;
                batteryPresent = true;
                QFile capacityFile(base + QStringLiteral("/capacity"));
                if (capacityFile.open(QIODevice::ReadOnly)) {
                    bool ok = false;
                    const int percent =
                        QString::fromLatin1(capacityFile.readAll()).trimmed().toInt(&ok);
                    if (ok && percent >= 0) {
                        chargeKnown = true;
                        charge = static_cast<double>(percent) / 100.0;
                    }
                }
            } else if (type == QStringLiteral("Mains") || type == QStringLiteral("USB")) {
                QFile onlineFile(base + QStringLiteral("/online"));
                if (onlineFile.open(QIODevice::ReadOnly)) {
                    bool ok = false;
                    const int online =
                        QString::fromLatin1(onlineFile.readAll()).trimmed().toInt(&ok);
                    if (ok) {
                        acKnown = true;
                        acOnline = acOnline || online != 0;
                    }
                }
            }
        }
        snapshot.power.batteryPresent =
            sentinel::core::measuredMetric(batteryPresent, capturedAt, hostOrigin,
                                           sentinel::core::ResourceReliability::Authoritative,
                                           QStringLiteral("linux.sysfs.power_supply"));
        if (acKnown) {
            const auto source = acOnline ? sentinel::core::ResourcePowerSource::Ac
                                         : sentinel::core::ResourcePowerSource::Battery;
            snapshot.power.powerSource = sentinel::core::measuredMetric(
                source, capturedAt, hostOrigin, sentinel::core::ResourceReliability::Authoritative,
                QStringLiteral("linux.sysfs.power_supply"));
        }
        if (batteryPresent && chargeKnown) {
            snapshot.power.batteryChargeFraction = sentinel::core::measuredMetric(
                charge, capturedAt, hostOrigin, sentinel::core::ResourceReliability::Authoritative,
                QStringLiteral("linux.sysfs.power_supply"));
        } else if (!batteryPresent) {
            snapshot.power.batteryChargeFraction = sentinel::core::unavailableMetric<double>(
                QStringLiteral("linux.sysfs.power_supply"), QStringLiteral("No battery present."));
        }
    }

    // --- thermal zones (temperature only; no portable native level) ---
    const QDir thermal(QStringLiteral("/sys/class/thermal"));
    double maxCelsius = -1.0;
    const auto zones = thermal.entryList({QStringLiteral("thermal_zone*")}, QDir::Dirs);
    for (const auto& zone : zones) {
        QFile temp(thermal.filePath(zone + QStringLiteral("/temp")));
        if (!temp.open(QIODevice::ReadOnly))
            continue;
        bool ok = false;
        const double milli = QString::fromLatin1(temp.readAll()).trimmed().toDouble(&ok);
        if (ok && milli > maxCelsius)
            maxCelsius = milli / 1000.0;
    }
    if (maxCelsius >= 0.0) {
        snapshot.thermal.temperatureCelsius = sentinel::core::measuredMetric<double>(
            maxCelsius, capturedAt, hostOrigin, sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("linux.sysfs.thermal"));
    } else {
        snapshot.thermal.temperatureCelsius = sentinel::core::unavailableMetric<double>(
            QStringLiteral("linux.sysfs.thermal"), QStringLiteral("No readable thermal zone."));
    }

    // --- GPUs (only independently measurable device-local values) ---
    const QDir drm(QStringLiteral("/sys/class/drm"));
    for (const auto& card : drm.entryList({QStringLiteral("card[0-9]*")}, QDir::Dirs)) {
        const QString device = drm.filePath(card + QStringLiteral("/device"));
        QFile totalFile(device + QStringLiteral("/mem_info_vram_total"));
        if (!totalFile.open(QIODevice::ReadOnly))
            continue;
        bool ok = false;
        const qint64 vramTotal = QString::fromLatin1(totalFile.readAll()).trimmed().toLongLong(&ok);
        if (!ok)
            continue;
        sentinel::core::GpuResourceMetrics gpu;
        gpu.deviceId = card;
        gpu.vramTotalBytes = sentinel::core::measuredMetric<qint64>(
            vramTotal, capturedAt, hostOrigin, sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("linux.sysfs.drm.vram_total"));
        QFile usedFile(device + QStringLiteral("/mem_info_vram_used"));
        if (usedFile.open(QIODevice::ReadOnly)) {
            bool usedOk = false;
            const qint64 used =
                QString::fromLatin1(usedFile.readAll()).trimmed().toLongLong(&usedOk);
            if (usedOk) {
                gpu.vramUsedBytes = sentinel::core::measuredMetric<qint64>(
                    used, capturedAt, hostOrigin,
                    sentinel::core::ResourceReliability::Authoritative,
                    QStringLiteral("linux.sysfs.drm.vram_used"));
            }
        }
        QFile busyFile(device + QStringLiteral("/gpu_busy_percent"));
        if (busyFile.open(QIODevice::ReadOnly)) {
            bool busyOk = false;
            const double busy = QString::fromLatin1(busyFile.readAll()).trimmed().toDouble(&busyOk);
            if (busyOk) {
                gpu.utilizationFraction = sentinel::core::measuredMetric<double>(
                    qBound(0.0, busy / 100.0, 1.0), capturedAt, hostOrigin,
                    sentinel::core::ResourceReliability::Approximate,
                    QStringLiteral("linux.sysfs.drm.gpu_busy_percent"));
            }
        }
        snapshot.gpus.append(gpu);
    }

    snapshot.thermal.level = sentinel::core::unsupportedMetric<QString>(
        QStringLiteral("linux.thermal.level"),
        QStringLiteral("No portable native thermal level on Linux; temperature is reported."));
    return snapshot;
}

ResourceCapabilityReport linuxCapabilities() {
    return buildCapabilities(
        QString::fromLatin1(kAdapterId),
        {{ResourceSignal::ProcessCpu, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessResidentMemory, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessVirtualMemory, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessThreadCount, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryTotal, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryAvailable, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryPressure, ResourceMetricAvailability::Measured},
         {ResourceSignal::BatteryPresence, ResourceMetricAvailability::Measured},
         {ResourceSignal::PowerSource, ResourceMetricAvailability::Measured},
         {ResourceSignal::BatteryCharge, ResourceMetricAvailability::Measured},
         {ResourceSignal::ThermalLevel, ResourceMetricAvailability::Unsupported},
         {ResourceSignal::ThermalTemperature, ResourceMetricAvailability::Measured},
         {ResourceSignal::GpuMemory, ResourceMetricAvailability::Measured},
         {ResourceSignal::GpuUtilization, ResourceMetricAvailability::Measured}});
}

#elif defined(Q_OS_WIN)

constexpr const char* kAdapterId = "windows.native";

double fileTimeSeconds(const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart) / 10'000'000.0;
}

HostResourceSnapshot sampleWindows(const ResourceSampleTimestamp& capturedAt) {
    HostResourceSnapshot snapshot;
    snapshot.adapterId = QString::fromLatin1(kAdapterId);
    snapshot.capturedAt = capturedAt;
    snapshot.hostLogicalCpuCount = QThread::idealThreadCount();
    const auto processOrigin = sentinel::core::ResourceMeasurementOrigin::SentinelProcess;
    const auto hostOrigin = sentinel::core::ResourceMeasurementOrigin::HostKernel;

    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        snapshot.process.cpuSecondsTotal = sentinel::core::measuredMetric<double>(
            fileTimeSeconds(kernel) + fileTimeSeconds(user), capturedAt, processOrigin,
            sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("windows.GetProcessTimes"));
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&memory), sizeof(memory))) {
        snapshot.process.residentBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(memory.WorkingSetSize), capturedAt, processOrigin,
            sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("windows.GetProcessMemoryInfo"));
        snapshot.process.virtualBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(memory.PrivateUsage), capturedAt, processOrigin,
            sentinel::core::ResourceReliability::Approximate,
            QStringLiteral("windows.GetProcessMemoryInfo"));
    }
    HANDLE threadSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnapshot != INVALID_HANDLE_VALUE) {
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        int count = 0;
        const DWORD self = GetCurrentProcessId();
        if (Thread32First(threadSnapshot, &entry)) {
            do {
                if (entry.th32OwnerProcessID == self)
                    ++count;
            } while (Thread32Next(threadSnapshot, &entry));
        }
        CloseHandle(threadSnapshot);
        snapshot.process.threadCount = sentinel::core::measuredMetric<int>(
            count, capturedAt, processOrigin, sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("windows.Toolhelp32"));
    }

    MEMORYSTATUSEX memoryStatus{};
    memoryStatus.dwLength = sizeof(memoryStatus);
    if (GlobalMemoryStatusEx(&memoryStatus)) {
        snapshot.hostMemory.totalBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(memoryStatus.ullTotalPhys), capturedAt, hostOrigin,
            sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("windows.GlobalMemoryStatusEx"));
        snapshot.hostMemory.availableBytes = sentinel::core::measuredMetric<qint64>(
            static_cast<qint64>(memoryStatus.ullAvailPhys), capturedAt, hostOrigin,
            sentinel::core::ResourceReliability::Authoritative,
            QStringLiteral("windows.GlobalMemoryStatusEx"));
        // dwMemoryLoad is a documented 0..100 percentage, not a native pressure
        // level, so it is reported as Approximate.
        snapshot.hostMemory.pressureLevel = sentinel::core::measuredMetric<double>(
            static_cast<double>(memoryStatus.dwMemoryLoad) / 100.0, capturedAt, hostOrigin,
            sentinel::core::ResourceReliability::Approximate,
            QStringLiteral("windows.GlobalMemoryStatusEx.dwMemoryLoad"));
    }

    SYSTEM_POWER_STATUS powerStatus{};
    if (GetSystemPowerStatus(&powerStatus)) {
        const auto platformOrigin = sentinel::core::ResourceMeasurementOrigin::PlatformService;
        const bool noBattery = (powerStatus.BatteryFlag & 128) != 0;
        snapshot.power.batteryPresent =
            sentinel::core::measuredMetric(!noBattery, capturedAt, platformOrigin,
                                           sentinel::core::ResourceReliability::Authoritative,
                                           QStringLiteral("windows.GetSystemPowerStatus"));
        if (powerStatus.ACLineStatus == 0 || powerStatus.ACLineStatus == 1) {
            const auto source = powerStatus.ACLineStatus == 1
                                    ? sentinel::core::ResourcePowerSource::Ac
                                    : sentinel::core::ResourcePowerSource::Battery;
            snapshot.power.powerSource =
                sentinel::core::measuredMetric(source, capturedAt, platformOrigin,
                                               sentinel::core::ResourceReliability::Authoritative,
                                               QStringLiteral("windows.GetSystemPowerStatus"));
        }
        if (!noBattery && powerStatus.BatteryLifePercent != 255) {
            snapshot.power.batteryChargeFraction = sentinel::core::measuredMetric(
                static_cast<double>(powerStatus.BatteryLifePercent) / 100.0, capturedAt,
                platformOrigin, sentinel::core::ResourceReliability::Authoritative,
                QStringLiteral("windows.GetSystemPowerStatus"));
        } else if (noBattery) {
            snapshot.power.batteryChargeFraction = sentinel::core::unavailableMetric<double>(
                QStringLiteral("windows.GetSystemPowerStatus"),
                QStringLiteral("No system battery."));
        }
    }

    snapshot.thermal.level = sentinel::core::unsupportedMetric<QString>(
        QStringLiteral("windows.thermal.level"),
        QStringLiteral("No unprivileged thermal level API wired for this build."));
    snapshot.thermal.temperatureCelsius = sentinel::core::unsupportedMetric<double>(
        QStringLiteral("windows.thermal.temperature"),
        QStringLiteral("No unprivileged thermal sensor API wired for this build."));
    return snapshot;
}

ResourceCapabilityReport windowsCapabilities() {
    return buildCapabilities(
        QString::fromLatin1(kAdapterId),
        {{ResourceSignal::ProcessCpu, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessResidentMemory, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessVirtualMemory, ResourceMetricAvailability::Measured},
         {ResourceSignal::ProcessThreadCount, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryTotal, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryAvailable, ResourceMetricAvailability::Measured},
         {ResourceSignal::HostMemoryPressure, ResourceMetricAvailability::Measured},
         {ResourceSignal::BatteryPresence, ResourceMetricAvailability::Measured},
         {ResourceSignal::PowerSource, ResourceMetricAvailability::Measured},
         {ResourceSignal::BatteryCharge, ResourceMetricAvailability::Measured},
         {ResourceSignal::ThermalLevel, ResourceMetricAvailability::Unsupported},
         {ResourceSignal::ThermalTemperature, ResourceMetricAvailability::Unsupported},
         {ResourceSignal::GpuMemory, ResourceMetricAvailability::Unsupported},
         {ResourceSignal::GpuUtilization, ResourceMetricAvailability::Unsupported}});
}

#else

constexpr const char* kAdapterId = "unsupported";

HostResourceSnapshot sampleUnsupported(const ResourceSampleTimestamp& capturedAt) {
    HostResourceSnapshot snapshot;
    snapshot.adapterId = QString::fromLatin1(kAdapterId);
    snapshot.capturedAt = capturedAt;
    snapshot.process.residentBytes = sentinel::core::unsupportedMetric<qint64>(
        QStringLiteral("unsupported"), QStringLiteral("No native telemetry adapter."));
    return snapshot;
}

ResourceCapabilityReport unsupportedCapabilities() {
    return buildCapabilities(QString::fromLatin1(kAdapterId), {});
}

#endif

class NativeResourceMonitorAdapter final : public sentinel::core::IResourceMonitorAdapter {
public:
    QString adapterId() const override {
        return QString::fromLatin1(kAdapterId);
    }

    ResourceCapabilityReport capabilities() const override {
#if defined(Q_OS_MACOS)
        return macOsCapabilities();
#elif defined(Q_OS_LINUX)
        return linuxCapabilities();
#elif defined(Q_OS_WIN)
        return windowsCapabilities();
#else
        return unsupportedCapabilities();
#endif
    }

    HostResourceSnapshot sample(const ResourceSampleTimestamp& capturedAt) const override {
#if defined(Q_OS_MACOS)
        return sampleMacOs(capturedAt);
#elif defined(Q_OS_LINUX)
        return sampleLinux(capturedAt);
#elif defined(Q_OS_WIN)
        return sampleWindows(capturedAt);
#else
        return sampleUnsupported(capturedAt);
#endif
    }
};

} // namespace

std::unique_ptr<IResourceMonitorAdapter> makeNativeResourceMonitorAdapter() {
    return std::make_unique<NativeResourceMonitorAdapter>();
}

} // namespace sentinel::core
