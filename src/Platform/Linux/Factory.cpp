#include "Platform/Factory.h"

#include "LinuxDiskProbe.h"
#include "LinuxGPUProbe.h"
#include "LinuxPathProvider.h"
#include "LinuxPowerProbe.h"
#include "LinuxProcessActions.h"
#include "LinuxProcessProbe.h"
#include "LinuxSystemProbe.h"
#include "Platform/IDiskProbe.h"
#include "Platform/IGPUProbe.h"
#include "Platform/IPathProvider.h"
#include "Platform/IPowerProbe.h"
#include "Platform/IProcessActions.h"
#include "Platform/IProcessProbe.h"
#include "Platform/ISystemProbe.h"

#include <memory>

namespace Platform
{

std::unique_ptr<IProcessProbe> makeProcessProbe()
{
    return std::make_unique<LinuxProcessProbe>();
}

std::unique_ptr<IProcessActions> makeProcessActions()
{
    return std::make_unique<LinuxProcessActions>();
}

std::unique_ptr<ISystemProbe> makeSystemProbe()
{
    return std::make_unique<LinuxSystemProbe>();
}

std::unique_ptr<IDiskProbe> makeDiskProbe()
{
    return std::make_unique<LinuxDiskProbe>();
}

std::unique_ptr<IPathProvider> makePathProvider()
{
    return std::make_unique<LinuxPathProvider>();
}

std::unique_ptr<IPowerProbe> makePowerProbe()
{
    return std::make_unique<LinuxPowerProbe>();
}

std::unique_ptr<IGPUProbe> makeGPUProbe()
{
    return std::make_unique<LinuxGPUProbe>();
}

} // namespace Platform
