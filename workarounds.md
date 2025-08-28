blurange needs an old version of gcc und gdb and thus cortex-debug, to allow different extension versions start vscode with `code . --extensions-dir .vscode-extensions`

if `compile_commands.json` is not automaticallt found, add this to `settings.json`:
`"C_Cpp.default.compileCommands": "${workspaceFolder}/_build/vscode/compile_commands.json",`