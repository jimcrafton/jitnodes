#pragma once

#include <map>
#include <string>
#include <vector>

#include <pybind11/pybind11.h>

namespace jitnodes::python {

// Adds the submodule `jitnodes.assets` (asset definitions, the library, renaming, the script check) and installs the check as the script
// validator of AssetDefinition (so AssetDefinition::validate, AssetLibrary::apply and save refuse scripts that break the rules).
void registerAssets(pybind11::module_& module);

// One thing wrong in an asset's scripts. `script` is the event or "module"; line and column are 1-based.
struct ScriptProblem {
    std::string script;
    int line = 1;
    int column = 1;
    std::string message;
};

// The rules for asset scripts, applied to the source of each (script name -> text; see assetScriptNames()):
//   every script: it must parse; exec, eval, compile and __import__ are refused, and getattr / setattr / delattr with a computed name;
//   onFrameChanged: it may not change the node (`kwargs['node']`, `node`, and anything derived from it) with a call such as set_float or
//     connect, an assignment, or a call of a `module` function that does;
//   onVersionUpdated: it may only read `old` and change `fresh`, and may not use the document (`doc`, `network`, `selection`, `curvy`).
// The check is by syntax and cannot see everything, so the handles enforce the same rule at run time (pyGuard.h). Needs the interpreter and
// the GIL; the application must have registered the `jitnodes` module.
std::vector<ScriptProblem> checkAssetScripts(const std::map<std::string, std::string>& scripts);

}  // namespace jitnodes::python
