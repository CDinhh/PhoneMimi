#pragma once

#include <opm/mcp/mcp_host.h>
#include <opm/mcp/mcp_protocol.h>

namespace opm::mcp {

// Every tool PhoneMimi offers. `host` must outlive the table.
ToolTable make_tools(Host& host);

} // namespace opm::mcp
