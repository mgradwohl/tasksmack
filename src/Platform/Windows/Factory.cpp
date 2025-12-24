#include "Platform/Factory.h"

#include "WindowsDiskProbe.h"
#include "WindowsProcessActions.h"
#include "WindowsProcessProbe.h"
#include "WindowsSystemProbe.h"

#include <memory>

namespace Platform
{

std::unique_ptr<IProcessProbe> makeProcessProbe()
{
    return std::make_unique<WindowsProcessProbe>();
}

std::unique_ptr<IProcessActions> makeProcessActions()
{
    return std::make_unique<WindowsProcessActions>();
}

std::unique_ptr<ISystemProbe> makeSystemProbe()
{
    return std::make_unique<WindowsSystemProbe>();
}

std::unique_ptr<IDiskProbe> makeDiskProbe()
{
    return std::make_unique<WindowsDiskProbe>();
}

} // namespace Platform
