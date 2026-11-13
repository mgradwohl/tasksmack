#pragma once

#include "Domain/SystemSnapshot.h"

namespace App::NumaNodesView
{

/// The CPU Cores tab's NUMA nodes card (#3088): "NUMA nodes: N" and a row per node with its processors,
/// their average use and its memory (NumaNodesText). Draws nothing unless the machine has more than one
/// node (NumaNodesText::showNumaNodes()). Reads only @p snap: no probe, no system call. Returns whether
/// it drew the card.
bool render(const Domain::SystemSnapshot& snap);

} // namespace App::NumaNodesView
