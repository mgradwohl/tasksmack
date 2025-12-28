#include "Platform/Factory.h"

#include "LinuxDiskProbe.h"
#include "LinuxPathProvider.h"
#include "LinuxProcessActions.h"
#include "LinuxProcessProbe.h"
#include "LinuxSystemProbe.h"

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

} // namespace Platform
