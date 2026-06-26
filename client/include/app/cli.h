// Folder Buddies — headless command-line front-end.
//
//   folderbuddies host <folder> [--lan] [--write] [--port N]
//   folderbuddies connect <room-code-or-offline-blob>
#pragma once

namespace fb {

bool is_cli_invocation(int argc, char** argv);
int run_cli(int argc, char** argv);

} // namespace fb
