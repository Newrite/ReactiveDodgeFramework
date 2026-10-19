# Behavior pipeline: Pandora native patches, hkx tooling, and the dodge-relevant graph map

Status: researched 2026-10-19 against Pandora source `1f26bbc` (`_ext/Pandora-Behaviour-Engine-Plus`, depth-1 clone +
submodules `HKX2-Enhanced-Library` and `XML.Cake.NET`) and the live STB MO2 profile. Nothing in MO2, Pandora or the
game was modified. Every claim is tagged **[src]** (read in Pandora source, file named), **[run]** (observed in the
real Pandora output / log on this machine) or **[inferred]** (not directly verified; says so).

Paths used below
- MO2 mods: `F:\STB\Skyrim\[STB] Mod Organizer\mods`
- Pandora install mod: `mods\Pandora Behaviour Engine Plus` (exe + `Pandora_Engine\Skyrim\Template` + Nemesis-style subfolders)
- Pandora OUTPUT mod: `mods\Pandora` (hkx under `meshes\actors\...`, `Engine.log`, `Pandora_Engine\ActiveMods.json`, `PreviousOutput.txt`) **[run]**
- Source tree: `S:\AI Agent Skyrim\ReactiveDodgeFramework\_ext\Pandora-Behaviour-Engine-Plus` (abbrev. `PBE`)
- Dumps: `_work\pandora_out_xml\` (current output, hkxc), `_work\vanilla_nemesis_xml\` (vanilla, Nemesis ids, hkxcmd)

---------------------------------------------------------------------------------------------------------------------

## 1. Tooling: hkx <-> XML

### 1.1 serde-hkx `hkxc` (built, working)
```
cd S:\Programming\serde-hkx
cargo build --release -p hkxc --ignore-rust-version      # crate is `hkxc` (crates/cli); NOT `hkx_cli`
# -> S:\Programming\serde-hkx\target\release\hkxc.exe   (hkxc 2.0.0, repo HEAD 3f53c059, 2026-08-11)
```
- The workspace declares `rust-version = 1.95`; the installed toolchain is 1.94.0. `--ignore-rust-version` builds fine in 75 s
  (no code needs 1.95). `rustup update` is the alternative, not needed. **[run]**
- Convert: `hkxc convert -i IN.hkx -o OUT.xml -v xml` and back `-v amd64` (SE, 64-bit) / `-v win32` (LE, 32-bit).
  NB: the CLI wants Windows paths (`F:/...`, not MSYS `/f/...`). **[run]**
- A second, older binary exists: `S:\SkyrimUtils\HKXNewTools\hkxc.exe` (hkxc 1.0.0). Same behaviour on everything below.

### 1.2 Format compatibility (all **[run]**)
| input | result |
|---|---|
| Pandora OUTPUT hkx (`mods\Pandora\meshes\...`; packfile `hk_2010.2.0-r1`, classversion 8, 64-bit LE, written by HKX2 `HKXHeader.SkyrimSE()`) | parses; all 14 dumped files converted |
| xml -> `-v amd64` -> xml again | **semantically identical** (object multiset with pointer ids normalised: equal, 2707 objects in 0_Master). Byte-identical hkx is NOT reproduced for the large HKX2-written files (`hkxc verify` reports a padding diff `expected Skip x64 ptr size(0 fill 8bytes)`); small files (blockbehavior, DefaultMale) verify clean. Do not use hkxc to *write* files the game will load without testing; use it as a reader. |
| Nemesis `cached_behaviors\*.hkx` (32-bit LE, vanilla Oldrim copies) | hkxc parses; hkxcmd parses |
| Nemesis `cached_behaviors\SSE\*.hkx` (real vanilla SE, 64-bit) | hkxc parses; **hkxcmd cannot** ("File is not loadable") |
| Pandora `Pandora_Engine\Skyrim\Template\...\*.hkx` (Pandora's own re-serialised vanilla; larger than the SSE cache, e.g. blockbehavior 204,864 B vs 193,568 B) | **hkxc fails** on both 2.0.0 and 1.0.0 (`expected Skip x64 ptr size`). Use `cached_behaviors\SSE` for SE vanilla via hkxc. |

### 1.3 Two id numberings - read this before writing a patch
- Pandora patches (and every Nemesis patch) address nodes with **Nemesis ids**: `#0340` = `RootBehaviorGraph` in 0_master.
  They are produced by `hkxcmd convert -v:xml` on `Nemesis_Engine\cached_behaviors\<file>.hkx` (32-bit copies).
  Proof they equal Pandora's numbering: Pandora's own `FNISParser.StateMachineMap` hard-codes `defaultmale~0_master -> #0340`
  **[src: PBE/Pandora.Skyrim/Format/FNIS/FNISParser.cs]**; that id is `RootBehaviorGraph` in the hkxcmd dump **[run]**;
  and the existing WBSP patch addressing `#0966/#0965/#0968` lands correctly in the current output **[run]**.
- hkxc (serde-hkx) numbers objects by **file layout order**, so its ids differ from Nemesis ids and also change whenever the file
  is re-serialised (`1HM_Behavior` is `#4876` in the Nemesis dump and `#0003` in the Pandora output). Use hkxc dumps only to
  verify by `class` + `name`, never to pick ids for a patch. **[run]**
- Vanilla SE (`cached_behaviors\SSE`, hkxc) and vanilla LE (hkxcmd) have the same object counts (0_master 2418 = 2418,
  mt 5115, 1hm 4794, block 895); ~1% of objects differ in field values (25/2418 in 0_master) **[run]**.
  Nemesis ids are therefore taken from the LE dump; the TUDM patch (`#0085/#0086/#0087` for 1hm tables) agrees with it.
- Commands that regenerate the dumps: `python tools\dump_pandora_out.py --vanilla --check` (see section 8).

---------------------------------------------------------------------------------------------------------------------

## 2. Pandora native patch grammar (verified from source)

Pandora loads three kinds of mods (**[src]** `PBE/Pandora.Core/Mods/ModLoaderService.cs`, `Providers/*.cs`):
1. **Pandora native**: `<DataDir>\Pandora_Engine\mod\<folder>\info.xml` + `patches\*.xml` (+ optional `native\<name>\<name>.dll`).
2. **Nemesis**: `<DataDir>\Nemesis_Engine\mod\<code>\info.ini` + per-node `#id.txt` files (what TUDM, XPMSE, Turn... use).
3. **FNIS** animation lists, scanned from `meshes\actors\<creature>\animations\<mod>\FNIS_<mod>_List.txt`.

If the same `code` exists in Pandora and Nemesis form, **Pandora wins** and the Nemesis one is dropped
(`ModLoaderService.LoadModsAsync`, merge key = (Code, Version)). **[src]** => one `code` cannot be both formats.

### 2.1 info.xml, code, priority, activation
```xml
<?xml version="1.0" encoding="utf-8"?>
<mod code="rdf">           <!-- XmlAttribute "code"; this is the id used for ActiveMods.json and node-name prefixes -->
  <name>Reactive Dodge Framework</name>   <!-- shown in UI and in every log line: Mod "<name>" -->
  <author>...</author>
  <site>null</site>
</mod>
```
**[src]** `PBE/Pandora.Core/Patch/Mod/PandoraModInfo.cs` (fields name/author/site + attribute code; `Version` is fixed 1.0.0;
`Priority`=0, `Active`=false by default), `Mods/Providers/PandoraModInfoProvider.cs` (`SingleRelativePath = Pandora_Engine\mod`,
info file `info.xml`, one folder level).
- There is **no priority field in info.xml**. Priority and on/off live in `Pandora_Engine\ActiveMods.json`
  (`[{"code","active","priority"}]`, written by the GUI; current file lists 26 mods, `wbsp` priority 24, the built-in `pandora`
  code is always forced active and sorted LAST: `ModInfoExtensions.NormalizePriorities`). **[src]+[run]**
- A **new mod code is inactive until switched on in the Pandora window** (`ModService.ApplySettings` only touches codes that
  already have a saved entry; new `PandoraModInfo.Active` defaults to false; `ModInfoViewModel` copies it). Lower priority applies
  first. **[src]** (the user must tick it once; editing the output mod's ActiveMods.json also works but is out of scope here).
- CLI flags (`PBE/Pandora.Skyrim/CLI/CommandLineParser.cs`): `--output/-o <dir>`, `--tesv/-tesv <skyrim dir>`, `--auto_run/-ar`,
  `--auto_close/-ac`, `--skyrim_debug64`. Useful for scripted runs through MO2.

### 2.2 Which packfile does `patches\<name>.xml` hit?
`PandoraAssembler.AssemblePackFilePatch` takes `Path.GetFileNameWithoutExtension(file)` and calls
`ProjectManager.TryActivatePackFilePriority(name)` -> `TryLookupPackFile(name.ToLower())` **[src]**
(`PBE/Pandora.Skyrim/Format/Pandora/PandoraAssembler.cs`, `Projects/ProjectManager.cs`, `Projects/Project.cs`).
- `0_master.xml`, `1hm_behavior.xml`, `mt_behavior.xml`, `blockbehavior.xml`, `sprintbehavior.xml`, ... : bare stem. Resolved via
  `_fileProjectMap` where **the first project that registers a stem wins** (`TryAdd`). Observed: bare `0_master.xml` hits the
  third-person graph (WBSP's event appears in the output `0_Master`). **[run]**
- `project~stem.xml` (tilde in the file name is legal): explicit project, e.g. `firstperson~0_master.xml`,
  `defaultmale~1hm_behavior.xml`. Required for anything first-person (`_1stperson\Behaviors\*` are separate files and
  projects). Log lines use exactly this `proj~file` form (e.g. `defaultfemale~1hm_behavior`). **[run]**
- Characters: `defaultmale_character.xml` / `defaultfemale_character.xml` (`Project.MapFiles` registers `<id>_character` and
  `<id>_skeleton`; Pandora's own exporter writes these names too: `PackFileChangeSet.SerializePandoraEdits`). Male and female are
  **separate files and projects** (siblings): patch both. `firstperson_character` exists as well. **[src]**
- Male/female third-person *behavior* files are one shared PackFile (same folder); a patch that "applies to both" is applied
  once per Nemesis folder, which is why TUDM's `defaultfemale~1hm_behavior ... Replace Text at #0426/modifiers FAILED` shows up
  (second application finds the text already replaced) - looks benign **[inferred]**.
- Only packfiles touched by some patch are exported (`ProjectManager.ActivePackFiles`). **[src]**

### 2.3 Operations and attributes
Root element name is arbitrary; Pandora recurses through any element that is not an operation
(`PandoraParser.ParseEdits`). Operation element names are case-insensitive (`Append` = `append`): `remove`, `insert`, `replace`,
`append` (`ChangeType` enum, `IPackFileChange.cs`). **[src]** `PBE/Pandora.Skyrim/Format/Pandora/PandoraParser.cs`
- An op with attributes is parsed directly; an op **without** attributes is a container whose children are ops of that type
  (`<append><x path="..">..</x></append>`).
- `path` attribute: `<nodeName>/<segment>/<segment>...`. The node name is everything before the first `/` (must contain a `/`,
  else `Parse > FAILED > Invalid format (missing '/')`). The node must exist (`Pop > FAILED > Not found in pack file`) or the
  whole op is dropped with a WARN.
- **Element ops** (child elements present):
  - `<append path="P">child...</append>` : add each child at the END of the element at `P` (`AppendElementChange`; `P` must exist).
  - `<insert path="P">child...</insert>` : insert each child **before** the element at `P`; if `P` does not exist the child is
    appended to `P`'s parent (`XMapElement.InsertElement`). Use `.../Element0` to put your entry FIRST.
  - `<replace path="P">child</replace>` : replace the element at `P` with the child (child must be a full element, e.g.
    `<hkparam name="startStateId">5</hkparam>`).
  - `<remove path="P"/>` : remove the element at `P`.
- **Text ops** (no child elements; used for pointer lists `#a #b #c` and number lists):
  - `<append path="P">#x</append>` : appends ` #x ` to the element text (whitespace-normalised) - pointer-list append.
  - `<replace path="P" skip="N"><old>#a</old><new>#b</new></replace>` : first occurrence of `old` at/after char offset `N`.
  - `<remove path="P" skip="N">#a</remove>` : remove first occurrence of the text after offset `N`.
  - `<insert path="P" index="N">#x</insert>` : insert text at char offset `N`.
  (`PackFileEditor.cs`; the char-offset forms are position dependent - prefer element ops.)
- **New nodes**: `<insert path="">` (or `<append path="">`) containing full `<hkobject name="#rdf$0" class=".." signature="0x..">`
  children; the `name` attribute is mandatory and must be unique in the packfile. A name that already exists logs
  `Track > FAILED > Already exists in pack file` and is skipped (`ParseEdit`, empty-path branch). Names are your choice
  (`#<code>$N` is the Nemesis convention); Pandora assigns final serial ids at export. Reference them from other nodes by that
  name (`#rdf$0`).

### 2.4 Path segments
Implemented by `XMapElement` (`PBE/XML.Cake.NET/XML Cake/Linq/XMapElement.cs`); every object is mapped by walking its XML:
- segment for an element WITH a `name` attribute = that name (`hkparam name="transitions"` -> `transitions`).
- segment for an element WITHOUT `name` (list entries, `<hkcstring>`, bare `<hkobject>`) = `Element<k>` where `k` = zero-based
  index among **all child elements of the parent**.
- Examples: `#0965/transitions` (list), `#0965/transitions/Element0/toStateId`, `#0106/eventNames/Element3`,
  `#2377/states` (pointer-list text), `#0340/startStateId`.
- Paths of list entries shift when earlier entries are inserted/removed; Pandora re-maps but other mods' edits are applied in an
  order you do not control (next section). Address by name wherever possible; avoid `ElementN` into lists others edit.

### 2.5 Application order (matters for indices and transition order)
**[src]** `PBE/Pandora.Skyrim/Hkx/Packfile/PackFileDispatcher.cs`, `Hkx/Changes/PackFileChangeSet.cs`, `PackFileGraph.cs`
1. Mod change sets are sorted by `Priority` ascending (higher number = later).
2. **Priority nodes first** (graph: `hkbBehaviorGraphData`, `hkbBehaviorGraphStringData`, `hkbVariableValueSet`; character:
   `hkbCharacterData`, `hkbCharacterStringData`): their edits are applied and pushed, then `PackFileValidator.ValidateEventsAndVariables`
   de-duplicates event/variable names (below). Within one change set the edits of one type to these nodes are applied in
   **reverse declaration order** (`ApplyForNode` loops `i = Count-1 .. 0`) - harmless when events/variables/infos are appended in
   parallel lists in the same order (all three get reversed alike), but do not rely on a particular appended index; resolve by name.
3. Everything else: **type-major across all mods**: all `remove`s (every mod), then all `insert`s, then `replace`s, then `append`s
   (`ChangeType` enum order Remove, Insert, Replace, Append; `ApplyInOrder`). So an `append` always lands after every `insert`.
4. Macros (next) are resolved after step 3 for every touched node and every new node.
5. `ApplyPatchesParallel` then scans FNIS lists, builds the OAR conversion for FNIS alternate animations and pushes XML back
   into objects (`PushXmlAsObjects`); export follows (`PackFileExporter`).

Consequence for state machines: your appended transition goes AFTER the vanilla/other-mod transitions in the same
`hkbStateMachineTransitionInfoArray`; for equal events the engine takes the first transition whose condition passes. Use
`<insert path="#id/transitions/Element0">` to get ahead of existing entries.

### 2.6 Macros for event / variable ids
**[src]** `PBE/Pandora.Skyrim/Hkx/Packfile/PackFileValidator.cs` (`TryGetNemesisEntity`, `ValidateElementContent`)
- Syntax: the **entire text of a leaf element** must be `$eventID[Name]$` or `$variableID[Name]$` (the parser keys on the 2nd char
  `e`/`v`, then `[`...`]`, then `$`). It is replaced with the integer index. No surrounding whitespace is trimmed - keep the macro
  on one line, glued to the tags. It does NOT work inside a multi-value text list (e.g. space-separated ints).
- Event names are matched **case-sensitively**, variable names **case-insensitively**.
- **A name that is not found becomes `-1` silently** (the "NOT FOUND" warning is in an unused helper). Result: a transition that
  never fires, no log line. Catch with `hkxcheck.py --require-event` and the `eventId -1` warning.
- Scope: macros are resolved only in (a) nodes that received any edit and (b) new nodes from `insert path=""`. A macro in an
  untouched node is left as literal text.
- Dedupe (`ValidateEventsAndVariables`): after the priority-node edits, an appended event/variable whose name already exists
  (vanilla or another mod) is **dropped** (`Validator > pkg~file > Duplicate Event > X > Index > N > SKIPPED`), and its
  `eventInfos`/`wordVariableValues`/`variableInfos` row with it; macros then resolve to the surviving index. Hence several mods can
  all "add" `RollStart` safely. Vanilla duplicates are left alone.

### 2.7 Adding an event / variable / character property
Events: the name must exist in **every graph file whose nodes use it and in `0_master`** (a sub-behavior's event is routed by name
through the root graph; event ids are per file: e.g. `RollStart` = 1285 in 0_Master, 964 in mt, 522 in 1hm **[run]**). Two lists
must grow together (Pandora does not pad `eventInfos`):
```xml
<append path="#0106/eventNames"><hkcstring>RDF_Dodge</hkcstring></append>          <!-- hkbBehaviorGraphStringData -->
<append path="#0108/eventInfos"><hkobject><hkparam name="flags">0</hkparam></hkobject></append>  <!-- hkbBehaviorGraphData -->
```
Variables: **three** parallel lists (all in the same order):
```xml
<append path="#0106/variableNames"><hkcstring>RDF_Id</hkcstring></append>
<append path="#0107/wordVariableValues"><hkobject><hkparam name="value">0</hkparam></hkobject></append>      <!-- hkbVariableValueSet; floats are stored as IEEE bits: 1.0f = 1065353216 -->
<append path="#0108/variableInfos"><hkobject><hkparam name="role"><hkobject><hkparam name="role">ROLE_DEFAULT</hkparam><hkparam name="flags">0</hkparam></hkobject></hkparam><hkparam name="type">VARIABLE_TYPE_INT32</hkparam></hkobject></append>
```
Types seen: REAL (91 in 0_master), BOOL (78), INT32 (56), VECTOR4 (3), QUATERNION (1), INT8 (1). VECTOR4/QUATERNION also need
`quadVariableValues` entries - not needed for a dodge framework. **Variable bounds**: `wordMinVariableValues` /
`wordMaxVariableValues` are empty (numelements 0) in every vanilla graph **[run]**, so no bounds are required.
**characterPropertyInfos**: only for variables bound as character properties (e.g. bone-mask names); lists in the graph
(`#0106/characterPropertyNames`, `#0108/characterPropertyInfos`) AND in the character file (`#0029/characterPropertyNames`,
`#0061/characterPropertyInfos`, `#0060/wordVariableValues`). Not needed for the planned work.
Variables are shared across sub-graphs by NAME (as events are), so a variable written by SKSE
(`SetGraphVariableInt`) must be declared in each graph file that binds it (TUDM does this: `DodgeID` is index 253 in 0_Master,
69 in mt, 107 in 1hm **[run]**).

Nemesis ids of the three tables per vanilla file ([run], hkxcmd dumps) - `eventNames/variableNames/characterPropertyNames` live in
STRING, `wordVariableValues` in VALUES, `eventInfos/variableInfos/characterPropertyInfos` in DATA:

| file | STRING | VALUES | DATA | root SM (Nemesis id) |
|---|---|---|---|---|
| 0_master | #0106 | #0107 | #0108 | Master_Behavior #2521 |
| mt_behavior | #0083 | #0084 | #0085 | MT_RootBehavior #5195 |
| 1hm_behavior | #0085 | #0086 | #0087 | 1HM_Behavior #4876 |
| 1hm_locomotion | #0063 | #0064 | #0065 | Melee_Locomotion_Behavior #0859 |
| blockbehavior | #0076 | #0077 | #0078 | BlockBehavior #0968 |
| bashbehavior | #0056 | #0057 | #0058 | BashBehavior #0184 |
| sprintbehavior | #0057 | #0058 | #0059 | SprintRootBehavior #0181 |
| staggerbehavior | #0052 | #0053 | #0054 | StaggerBehaviorRoot #0161 |
| idlebehavior | #0062 | #0063 | #0064 | Behavior00 #1440 |
| weapequip | #0057 | #0058 | #0059 | WeapEquipRoot #0300 |
| magicbehavior | #0077 | #0078 | #0079 | MagicBehavior #1346 (child MagicRootBehavior #1336) |
| characters\DefaultMale / DefaultFemale | hkbCharacterStringData #0029 (animationNames) | hkbVariableValueSet #0060 | hkbCharacterData #0061 | - |

### 2.8 New animation clips (.hkx) - what is and is not supported
1. **Character `animationNames`** (list of `Animations\...hkx` strings relative to `meshes\actors\character\`): **yes, patchable**:
   `defaultmale_character.xml` / `defaultfemale_character.xml`, node `#0029`:
   `<append path="#0029/animationNames"><hkcstring>Animations\RDF\Roll_Fwd.hkx</hkcstring></append>`.
   `hkbClipGenerator.animationName` must equal one of these strings (case-insensitive). The current output char files have 1770
   names (1656 vanilla + 114 mods; TUDM adds `Animations\Roll_*.hkx`, `Sidestep_*.hkx`). Pandora does not dedupe character
   names for native patches - use a unique subfolder (`Animations\RDF\`). `hkxcheck.py --character` verifies clip names.
2. **Animation data** (`meshes\animationdatasinglefile.txt`, `animationsetdatasinglefile.txt`; per-project clip blocks keyed by the
   *clip generator name* with annotation/trigger strings `Name:time` + motion data = root motion): Pandora's native format has
   **no hook for it** - `PandoraAssembler.AssemblePatch` handles only `patches\*.xml` and `native\`. Only Nemesis-format mods
   carry them (`Nemesis_Engine\mod\<code>\animationdatasinglefile\<Project>~<n>\<Clip>~<id>.txt` + `<id>.txt` motion block;
   `animationsetdatasinglefile\<Project>Data~<Project>\*.txt`), see `NemesisAssembler.AssembleAnimDataPatch/AssembleAnimSetDataPatch`.
   TUDM's blocks look like: `ForwardRoll_Clip_B / tudm$9 / 1 / 0 / 0 / 4 / RollNext:0.3 / RollWinStart:0.75 / RollWinEnd:0.80 /
   RollNext:0.85`, motion block: `tudm$0 / 0.933333 / 6 / <time x y z>...`. **[run]**
   Consequence: a clip with root motion needs a Nemesis-format companion mod (different `code` from the Pandora patch), or the
   Pandora **native C# plugin** route (below), or a motion-free approach (SKSE-driven displacement).
3. **FNIS-style lists** (`PBE/Pandora.Skyrim/Format/FNIS/*`): `meshes\actors\character\animations\<Mod>\FNIS_<Mod>_List.txt` AND
   `meshes\actors\character\behaviors\FNIS_<Mod>_Behavior.hkx` (must pre-exist; Pandora does not create it) are both required for
   humanoids (`TryFindAnimlistHumanoid/TryFindGraphHumanoid`). Then Pandora adds each listed .hkx path to the character
   (`BasicAnimation.BuildAnimation -> CharacterPackFile.AddUniqueAnimation`, male AND female via sibling), and injects a
   `PN_StateInfo` (stateId = xxHash32(graph path) & 0x7FFFFFFF) with a `hkbBehaviorReferenceGenerator` into the root state machine
   (`#0340` of 0_master; `StateMachineMap`). It does not add animdata (the `AddDummyClipData` call is commented out). The log's
   `FNIS Parser > Scan FNIS Animations > TKDodge in defaultmale > FAILED` just means no list/graph in that folder (TK Dodge is
   not installed as a behavior mod here). 40 such lines are present and harmless. **[src]+[run]**
4. **Native C# plugin** (`Pandora_Engine\mod\<code>\native\<name>\<name>.dll` containing `ISkyrim64Patch` classes with
   `Mode` Serial/Parallel and `Order` PreLaunch/PostLaunch; `PBE/Pandora.Example.Plugin`): receives `IProjectManager`; can call
   `project.CharacterPackFile.AddUniqueAnimation(..)` and `project.AnimData.AddClipData(IClipDataBlock, IClipMotionDataBlock)`.
   This is the only Pandora-native way to ship animdata. It requires building a .NET plugin against `Pandora.API`.

### 2.9 Outputs, logging, and detecting failed edits
- Output tree: `<Output>\meshes\actors\...\<same relative path as in Template>` for every *touched* packfile, plus merged
  `meshes\animationdatasinglefile.txt`, `meshes\animationsetdatasinglefile.txt`, `SKSE\Plugins\fnis_aa\config.json`,
  `Pandora_Engine\ActiveMods.json`, `PreviousOutput.txt` (list of files written; they are deleted at the start of the next run:
  `BasePackFileExporter.LoadMetaData`), and **`Engine.log`** (NLog, layout `LEVEL : message`, overwritten each run,
  `UserLogPathProvider` = output folder). **[src]+[run]**
- Export writes `HKXHeader.SkyrimSE()` (64-bit packfile). If binary export throws, Pandora logs `Export > proj~file > FAILED`
  (FATAL) and writes an **XML file at the .hkx path** (`PackFileExporter.Export`) - a file starting with `<?xml` in the output is
  a failed export.
- Log patterns to grep (also implemented by `tools\pandora_log.py`):
  - `Dispatcher > Mod "X" > PackFile "proj~file" > Apply > FAILED > <Append|Insert|Replace|Remove> <Element|Text> at <path>` : the
    edit did not apply (path missing, text not found, parent absent). Other edits continue; **nothing is reverted**.
  - `Pandora Parser > Mod "X" > Node "#id" > Pop > FAILED > Not found in pack file` : wrong node id (or wrong file) -> whole op skipped.
  - `... Path "p" > Parse > FAILED > Invalid format (missing '/')`; `... Track > FAILED > Already exists in pack file` (name clash).
  - `Pandora Assembler > Mod "X" > File ".." > Load > FAILED` : patch XML not well-formed (whole file ignored).
  - `Validator > ... Duplicate Event|Variable ... SKIPPED` : de-dup (usually fine).
  - `Packfile > Active Nodes > Push > FAILED` : a node could not be rebuilt from XML (that node's edits are lost).
  - the closing `INFO : Pandora Mod N : <name>` lines list which mods ran (a missing line = mod inactive in the GUI).
- **Silent failures (no log line)**: unknown `$eventID[..]$`/`$variableID[..]$` -> `-1`; a pointer `#name` that resolves to nothing
  becomes `null` (`HavokXmlDeserializer.ReadClassPointer` returns default; `ReadClassPointerArray` skips it); a pointer-list
  `append` onto a list whose `numelements="0"` is ignored because `ReadClassPointerArray` returns early when the attribute is 0
  (**[inferred]** from `HKX2/HavokXmlDeserializer.cs:474-480`; replace the whole `hkparam` with a correct `numelements` instead).
  Struct-array appends are safe: `ReadClassArray` only requires `numelements` to parse and iterates the real children.
- Verification loop: run Pandora from MO2 -> `python tools\pandora_log.py --mod "Reactive"` -> `python tools\dump_pandora_out.py --check`
  -> `python tools\hkxcheck.py _work\pandora_out_xml\1hm_behavior.xml --require-event RDF_Dodge --character _work\pandora_out_xml\DefaultMale.xml`.

### 2.10 Minimal worked example (pattern from WBSP, verified to land)
`Pandora_Engine\mod\rdf\info.xml` as above and `patches\blockbehavior.xml`:
```xml
<patch>
  <append path="#0076/eventNames"><hkcstring>RDF_Test</hkcstring></append>
  <append path="#0078/eventInfos"><hkobject><hkparam name="flags">0</hkparam></hkobject></append>
  <insert path="">
    <hkobject name="#rdf$0" class="hkbBlendingTransitionEffect" signature="0xfd8584fe"> ... </hkobject>
  </insert>
  <append path="#0968/states">#rdf$1</append>                       <!-- pointer-list text append -->
  <insert path="#0965/transitions/Element0"> <hkobject> ... <hkparam name="eventId">$eventID[RDF_Test]$</hkparam> ... </hkobject> </insert>
</patch>
```
Reference implementation in this machine's WeaponBlockSprint: `S:\Programming\WeaponBlockSprint\tools\gen_pandora_patch.py`
(the resulting transition is visible in the current output: `hkxcheck.py blockbehavior.xml --find-event WeaponBlockSprint` **[run]**).

### 2.11 Trap list (short)
1. New mod = inactive in the Pandora GUI until ticked.  2. Bare stem hits the first-registered project; first person needs
`firstperson~...`.  3. hkxc ids != Nemesis ids.  4. Missing macro name -> -1 silently.  5. Event/variable must be in 0_master and
in each file that uses it; add to all parallel lists.  6. Same-type ops are type-major across mods; appended transitions come
last.  7. stateIds must be unique per state machine: occupied now - 1hm: 0,1,2,8,9,10,12,13,18,19,25,26(TUDM),30,31(stances),
138-140,420(Precision); mt: 0,4,15,18,19,20,25,26(TUDM),51,59,60,70,72-75,80-84,118; pick hashed/high ids (FNIS uses
xxHash32 & 0x7FFFFFFF).  8. Appending pointers to an empty (numelements=0) pointer list is ignored.  9. Animation names: unique
subfolder; patch BOTH male and female character files; animdata needs Nemesis format or a native plugin.

---------------------------------------------------------------------------------------------------------------------

## 3. Structure of the character graph (vanilla = Nemesis ids; current = Pandora output, hkxc ids)

All of this **[run]** (`hkxcheck.py --summary` / `--list-sm` on the dumps).

### 3.1 Hierarchy
```
0_master (hkbBehaviorGraph #2522, root = Master_Behavior SM #2521, 10 states, wildcard array #0110)
 |- state 0 Root -> RootModifierGenerator -> RootBehaviorGraph SM #0340 (34 vanilla states, startStateId 14, wildcard array #0341, 32 wildcards)
 |    |- 14 Default_State -> Default_Behavior SM #0721 (9 states, wildcard #0722)          <- normal gameplay
 |    |     |- 0  MT_Behavior_State -> MT_LeftHandOverride SM #2039 -> BFR MT_BehaviorBFR -> Behaviors\MT_Behavior.hkx  (no weapon / locomotion)
 |    |     |- 1  Weap_Readied_State -> BFR 'Weap_BehaviorBFR' -> Behaviors\1HM_Behavior.hkx           (weapon drawn: attack, block, bash, bow...)
 |    |     |- 2  Magic_Behavior_State -> MRhLeftHandOverride SM #0863 -> BFRs Magic_BFR -> Behaviors\MagicBehavior.hkx
 |    |     |- 10 Weap_Equip_State, 11 Magic_Equip_State, 12 Weap_Unequip_State, 13 Magic_Unequip_State (equip/unequip chain)
 |    |     |- 14 KillMove_State, 20 NonLethal_PairedAnimations
 |    |- 0..5 Jump states (JumpStanding, JumpFastDirectional, JumpFall*, JumpLand*), 7 Stagger_State, 48 PlayerStagger_State,
 |    |  42 toForceEquipState, 8 BleedOut, 38 Swim, 21 HorseRider, 6 RitualSpell, 10 FullBodyPlayerIdles, 43-45/49/52/53 paired+mount, 131-140/170 killmove/vampire/dragon
 |- 21..24 Cart, AnimateToRagdoll, FullyRagdoll, GetUpFromRagdoll; 48-50,115,122 transformations/cinematics
```
Current output adds to this: Root SM state **20 `ItemUse_Potion_State`** (BehaviorReferenceGenerator `Behaviors\ItemUse.hkx`, mod
`itmuse`), states **171-174 JumpLandImpact/HighImpact** (`jpbo`), Master_Behavior state **453 `SkyParkourState`** (`sppftp`), and 4 more
root wildcards (36 vs 32). `Default_Behavior` is unchanged (still 9 states). 0_Master has 1300 events (vanilla 1217) and 272 variables
(230), mt 965/70 (931/67), 1hm 523/112 (461/101).

Where things live (this corrects the usual assumption that attacks are BFR-loaded - they are inline in 1hm_behavior):
| function | graph / node (Nemesis id for vanilla, state ids) |
|---|---|
| locomotion, weapon sheathed | mt_behavior: `MT_RootBehavior` #5195, state 0 `MT_State` #5194 -> `DefaultOrDrunkBehavior` #5190 -> `MT_Default_Behavior` (moveStart/moveStop/turn states). Idles via BFR `IdleBFR` -> IdleBehavior.hkx, sprint via BFR `SprintBFR` inside `Sprint_State`/`Sneak_Sprint_State` |
| locomotion, weapon drawn | 1hm_behavior state 0 `1HM_Ready_State` #4874 (`1HM_Ready_State_MG`) -> `Weapon_Locomotion_BFR` -> 1HM_Locomotion.hkx (`Melee_Locomotion_Behavior`, its own `sprintBFR` state `Melee_Sprint_State`) |
| attack (melee) | 1hm_behavior state 10 `AttackState` #4806 (gen `1HM_AttackState_Generator`; inline SMs `1HM_AttackBehavior`, power-attack SMs); bow state 18, crossbow 139/140, recoil 8/9, killmove 12, shout 13 (BFR Shout_Behavior.hkx), stance-change |
| block | 1hm_behavior state 1 `BlockState` #2801 -> SM `BlockBehavior` #2789 (the `blockbehavior` file is a *separate* graph via `BlockBFR` #2780; WBSP hooks its root) |
| bash | 1hm_behavior state 2 `Bash_State` + `BashBFR` -> BashBehavior.hkx |
| sprint (own graph) | SprintBehavior.hkx, referenced by `SprintBFR` in mt (states 'Sprint_State' stateId 1), 1hm (`1HM_1stP_SprintState`, `H2H_SprintPoseState`), 1hm_locomotion, blockbehavior (`BlockSprint`) |
| stagger | 0_master Root SM: wildcard `staggerStart` (79) -> state 7 `Stagger_State`; `StaggerPlayer` (1072) -> 48 `PlayerStagger_State`; `StaggerBFR` -> StaggerBehavior.hkx (inside `toForceEquipState`); in 1hm: wildcard `recoilStart` -> 8 `Recoil`, `recoilLargeStart` -> 9 |
| magic | Default_Behavior state 2 -> MagicBehavior.hkx (`MagicRootBehavior`: ReadyState 0, CastingState 1) |
| **dodge (TUDM, current)** | state 25 `DodgeState` + 26 `Post_DodgeState` added to the root SM of **1hm_behavior, mt_behavior, magicbehavior** (+ `_1stperson` copies in the Nemesis patch); events/variables declared in 0_master too |

Vanilla state tables with `stateId`, state node id and its `transitions` array id (needed as `append`/`insert` targets):
- **1HM_Behavior #4876** (wildcard array **#0088**, 10 wildcards): 0 `1HM_Ready_State` #4874 (trans #4872) / 1 `BlockState` #2801 (#2792) /
  2 `Bash_State` #2777 (#2771) / 8 `Recoil` #4867 (#4864) / 9 `RecoilLarge` #4825 (#4822) / 10 `AttackState` #4806 (#4802) /
  12 `KillMoveState` #1884 (#1879) / 13 `1HM_Shout_State` #2765 (#2763) / 18 `Bow_AttackState` #2761 (#2758) / 19 `CombatIdles` #2612
  (#2609) / 138 `VictimState` #2527 / 139 `CrossBow_AttackState` #0446 (#0443) / 140 `CrossBow_ReloadState` #0259 (#0256).
- **MT_RootBehavior #5195** (wildcard array **#0089**, 143 wildcards): 0 `MT_State` #5194 (trans #5192) + 19 more utility states.
- **Master_Behavior #2521** wildcard #0110 (6); **RootBehaviorGraph #0340** wildcard #0341 (32); **Default_Behavior #0721** wildcard #0722 (7).
- **MagicRootBehavior #1336**: 0 `MagicReadyState` #1334 (trans #1333), 1 `CastingState` #1023 (#1022).

### 3.2 Wildcard (global) transitions
- RootBehaviorGraph wildcards (#0341) use flags `IS_LOCAL_WILDCARD` and/or `IS_GLOBAL_WILDCARD` with `DISABLE_CONDITION`: Horse
  enter, `staggerStart`->7 (with condition #0349), `WardBreak`->7, jump starts/lands, `SwimStart`->38, `bleedOutStart`->8, idle
  events -> 10 `FullBodyPlayerIdles`, `Ragdoll`->14, `JumpFall*`, `pa_VampireFeed*`, `KillMove*`, `DragonMount*`,
  `VampireLordChangePlayer`. `JumpLandEnd` (638) returns to 14 `Default_State` through four conditioned wildcards.
- Default_Behavior wildcards (#0722): `MagicForceEquip*`, `WeapOut*ReplaceForceEquip`, `MagicWeap_ForceEquip` (-> equip states 10/11,
  flags include `TO_NESTED_STATE_ID_IS_VALID|IS_LOCAL_WILDCARD|IS_GLOBAL_WILDCARD|ALLOW_SELF_TRANSITION_BY_TRANSITION_FROM_ANY_STATE`)
  and `ReanimateExit` (1066) -> state 0 MT.
- 1HM_Behavior wildcards (#0088): `recoilStart/LargeStart` -> 8/9 (`fastBlendTransition`, `IS_LOCAL_WILDCARD|DISABLE_CONDITION`),
  `IdleCombat*` -> 19, `streamingQuery_attackStart` -> 12, `Collision_Recoil` -> 420 (Precision).
- MT_RootBehavior wildcard (#0089, 143): includes **`IdleForceDefaultState` (130) -> state 0 `MT_State`** with `DefaultBlendResetTransition`
  (0.2 s, selfTransitionMode RESET) - this is how idles/furniture/etc. drop back to locomotion in the sheathed graph.
- Transition flag frequency over vanilla 0_master/mt/1hm/block/sprint/stagger: `DISABLE_CONDITION` 955, `LOCAL|GLOBAL|DISABLE_CONDITION` 299,
  `LOCAL|DISABLE_CONDITION` 127, `TO_NESTED_STATE_ID_IS_VALID|DISABLE_CONDITION` 122, `0` 98, `DISABLE_CONDITION|USE_INITIATE_INTERVAL` 27.

### 3.3 Common transition effects (hkbBlendingTransitionEffect; counts = transitions using each variant, vanilla)
`duration / toGeneratorStartTimeFraction / flags / endMode / blendCurve / selfTransitionMode / eventMode`:
| n | duration | flags | selfTransitionMode | eventMode | typical names |
|---|---|---|---|---|---|
| 271 | 0.5 | 0 | CONTINUE_IF_CYCLIC_BLEND_IF_ACYCLIC | PROCESS_ALL | `0.5secondBlend` |
| 240 | 0.2 | 0 | CONTINUE_IF_CYCLIC_BLEND_IF_ACYCLIC | DEFAULT | `DefaultBlendTransition` |
| 144 | 1.0 | 0 | CONTINUE_IF_CYCLIC_BLEND_IF_ACYCLIC | DEFAULT | `1secondBlend` |
| 130 | 0.0 | 0 | BLEND | DEFAULT | `attackStartTransition`, `DefaultBlendTransition` |
| 129 | 0.2 | 0 | BLEND | DEFAULT | `DefaultBlendTransition` |
| 79 | 0.2 | FLAG_IGNORE_FROM_WORLD_FROM_MODEL | CONTINUE_IF_CYCLIC_BLEND_IF_ACYCLIC | PROCESS_ALL | `...ToAnimDriven` |
| 75 | 0.033 | 0 | CONTINUE_IF_CYCLIC_BLEND_IF_ACYCLIC | PROCESS_ALL | `instantBlendTransition` |
| 74 | 0.0 | FLAG_SYNC | CONTINUE_IF_CYCLIC_BLEND_IF_ACYCLIC | DEFAULT | `DefaultSyncBlendTransition` |
All have `toGeneratorStartTimeFraction 0.0`, `endMode END_MODE_NONE`, `blendCurve BLEND_CURVE_SMOOTH`. Into/out of animation-driven (root
motion) states vanilla sets `FLAG_IGNORE_FROM_WORLD_FROM_MODEL` / `FLAG_IGNORE_TO_WORLD_FROM_MODEL` (hence names `...ToAnimDriven`,
`...FromAnimDriven`). TUDM: `DodgeRollTransition`, `DodgeEndTransition`, `PostDodgeRollChange_Transition` = **0.1 s, flags
`FLAG_IGNORE_FROM_WORLD_FROM_MODEL`, SMOOTH, selfTransitionMode CONTINUE / CONTINUE_IF_CYCLIC_BLEND_IF_ACYCLIC**. Pandora's own FNIS
transition: 0.6 s `DefaultTransition_0.6` (`BasicAnimation`).

---------------------------------------------------------------------------------------------------------------------

## 4. How the existing dodge (TUDM, "The Ultimate Dodge Mod" by Shikyo Kira) is wired - current Pandora output

TUDM reaches Pandora as a **Nemesis-format** mod (`(TUDM) The Ultimate Dodge Mod Pandora Fix`, `Nemesis_Engine\mod\tudm`: whole-node
files `#0106.txt`, `#tudm$0.txt`...; also `animationdatasinglefile\DefaultMale~1\...`, character `defaultmale\#0029.txt` replacing the
full animationNames list). Pandora parses it with `NemesisParser`. TK Dodge / DMCO / MCO events are **not present** in the graphs
(`TKDodge*`, `MCO*`, `BFCO*` have no matching event). Events present: `DodgeRight/Left/Back` (vanilla, 953-955 in master, unused), `TDM_Dodge`
is only a declared variable in 0_master (256), `TDM_AttackStart` a declared event.

**Events** (ids master / mt / 1hm): `RollStart` 1285/964/522, `RollStop` 1284/963/521, `RollContinue` 1281/960/518, `RollWinStart`
1283/962/520, `RollWinEnd` 1282/961/519, `RollTrigger` 1279/958/516, `RollLeap` 1280/959/517, `RollLand` 1253/941/490,
`ForwardRollStart` 1278/957/515 and the 7 other `<dir>RollStart` (`Left_ForwardRollStart`..`Right_ForwardRollStart`), their
`Post_<dir>RollStart` (8), `SidestepTrigger` 1250/938/487, `FootLeftSidestep`/`FootRightSidestep`, plus `DodgeMasterSpell` 1286,
`DodgeCharge` 1287, `DodgeLightningStorm` 1288 (master only, fired by clip triggers in the Nemesis `#tudm$0` array).
**Variables** (master / mt / 1hm): `DodgeSpeed` 252/68/106 (bound to clip `playbackSpeed`), `DodgeID` 253/69/107 (bound to nested
`<dir>DodgeStyle` SMs' `startStateId`: picks roll vs sidestep), `IsDodging` -/67/102 (set to 1 by `Dodging_ExpressionModifier`,
expression `IsDodging = 1`), `Direction` (drives `fBlendParameter` of `BSCyclicBlendTransitionGenerator`), `bAllowRotation`
(bound to `BSIsActiveModifier 'DodgeRollAllowRotationModifier'`). They are written by the mod's Papyrus/SKSE side
(`SendAnimationEvent("RollStart")` after setting variables) **[inferred]**.

**Graph shape** (same in 1hm_behavior, mt_behavior, magicbehavior; ids below are the current-output hkxc ids of 1hm_behavior):
- Root SM `1HM_Behavior` (#0003 in output) gets **state 25 `DodgeState`** and **26 `Post_DodgeState`**
  (`MT_RootBehavior` and `MagicRootBehavior` likewise).
- Entry transitions `RollStart` -> toStateId 25, effect `DodgeEndTransition` (0.1 s) from: 1HM_Ready_State(0, `DISABLE_CONDITION`),
  BlockState(1), Bash_State(2, `USE_INITIATE_INTERVAL`), AttackState(10, **with conditions** - several `USE_TRIGGER_INTERVAL|
  USE_INITIATE_INTERVAL` entries that gate on attack phase), Bow_AttackState(18), CrossBow_ReloadState(140). **Not** from Recoil(8),
  RecoilLarge(9), KillMove(12), Shout(13), CombatIdles(19). In mt only from `MT_State`(0) (one transition covers the whole locomotion
  machine incl. sprint); in magic from MagicReadyState(0) and CastingState(1).
- State 25 enter events: `blockStop` (5), `SoundStop.WPNBowPullLPM`. Its generator = `ModifierGenerator08` =
  [`Dodging_ExpressionModifier`, nested SM `DodgeDirection` #5088].
- `DodgeDirection` SM: **state 0 `DodgeDirection_StateA`** (`BSCyclicBlendTransitionGenerator` blending 8 direction children on `Direction`;
  each child is an SM `<dir>DodgeStyle` with `startStateId <- DodgeID` choosing between clip `Animations\Roll_<dir>.hkx` and
  `Sidestep_<dir>.hkx`, mode LOOPING, triggers `RollTrigger`/`SidestepTrigger` @0.1 s and `<dir>RollStart` @0.14-0.175 s) ->
  event `<dir>RollStart` (8 transitions, `DISABLE_CONDITION|TO_NESTED_STATE_ID_IS_VALID`, no effect) -> **state 1 `DodgeDirection_StateB`**
  (`DodgeDirection_MG`: `BSIsActiveModifier` for bAllowRotation + SM `DodgeDirection_BehaviorB` with the real clips, mode SINGLE_PLAY,
  `playbackSpeed <- DodgeSpeed`).
- Clip B triggers (not annotations: `isAnnotation=false`) e.g. roll forward: `RollLeap`@0.3, `RollLand`@0.5, **`RollWinStart`@0.7,
  `RollWinEnd`@0.8** (the invulnerability window, consumed by the Papyrus/SKSE side), `Post_ForwardRollStart`@0.85, `RollStop`
  @ end-of-clip (`relativeToEndOfClip=true`, localTime 0). Sidestep clips add `FootRight/LeftSidestep`.
  Animdata (Nemesis blocks) lists the same clip names with annotation strings (`RollNext:0.3`, `RollWinStart:0.75`...).
- Leaving: in state 25 `Post_<dir>RollStart` (8 events, `DISABLE_CONDITION|TO_NESTED_STATE_ID_IS_VALID`) -> 26 `Post_DodgeState`
  (chain window); in 26: `RollStart` -> 25 (`PostDodgeRollChange_Transition`, 0.1 s; chained dodge), `RollStop` -> 0 `1HM_Ready_State`
  (condition #5083) or -> 1 `BlockState` (condition #5084) with `DodgeEndTransition`; and from both 25 and 26 every action start
  (`shoutStart`, `blockStart`, all `attackStart*`/`attackPowerStart*`, `bowAttackStart`, `streamingQuery_attackStart*`, H2H starts) -> its
  state with `DodgeEndTransition` and `USE_INITIATE_INTERVAL` - i.e. dodge-cancel into attack/block is implemented by duplicating the
  whole attack-start transition list on the dodge states (dozens of transitions per dodge state, per graph).
- Other mods hooking the same SM: Precision (`Collision_Recoil` wildcard -> state 420 `Precision_Recoil_State`), stance change
  (`LeftStance`/`RightStance` -> 30/31), True Directional Movement (`TDM_AttackStart`, `TDM_LockRotation`).

**Master-level**: 0_master contains no dodge states; only the event/variable declarations (so the engine routes `RollStart` and friends to the
sub-graphs). Return to locomotion from an action: sheathed = wildcard `IdleForceDefaultState` in `MT_RootBehavior` (not used by dodge; dodge
returns through its own `RollStop`->state 0), drawn = per-action stop event to `1HM_Ready_State` (`RollStop`, `attackStop`, `bashExit`,
`blockStop`, `recoilStop`, `shoutStop`, `IdleStop`), and in 0_master the equip chain (`WeapEquip_Out(49)` -> `Weap_Readied_State`,
`Unequip_Out(50)` -> `MT_Behavior_State`), `ReanimateExit` and `JumpLandEnd` (wildcards to state 14). `moveStart/moveStop/turn*` are NOT global:
they are used inside the locomotion sub-machines (`MT_Default_Behavior` standing<->moving; crossbow/bow lower-body machines in 1hm).

### 4.1 Event ids in the key graphs (vanilla; identical in the current output for these names)
| event | 0_master | mt | 1hm | block | sprint | stagger |
|---|---|---|---|---|---|---|
| moveStart | 24 | 31 | 17 | 8 | - | - |
| moveStop | 25 | 32 | 18 | 7 | - | - |
| IdleForceDefaultState | 496 | 130 | - | - | - | - |
| attackStart | 22 | - | 11 | - | - | - |
| attackStop | 23 | - | 8 | 34 | 13 | - |
| blockStart | 18 | - | 10 | 26 | - | - |
| blockStop | 19 | - | 5 | 24 | - | - |
| staggerStart | 79 | - | 51 | - | - | 3 |
| staggerStop | 80 | - | 2 | - | - | 0 |
| SprintStart | 12 | 34 | 20 | 15 | - | - |
| SprintStop | 13 | 33 | 19 | 14 | 9 | - |
| turnStop | 35 | 39 | 23 | 9 | - | - |
| turnLeft / turnRight | 39 / 38 | 40 / 41 | 25 / 24 | 10 / 11 | - | - |
| EnableBumper / DisableBumper | 931 / 930 | - | - | - | - | - |
| JumpLandEnd | 638 | - | - | - | - | - |
| MTState | 1098 | - | - | - | - | - |
| WeapEquip / Unequip | 2 / 144 | - | - | - | - | - |
| StaggerPlayer | 1072 | - | - | - | - | - |
| bashStart / bashRelease | 93 / 145 | - | 43 / 52 | - / 18 | - | - |
| DodgeRight/Left/Back (vanilla, unused) | 953-955 | - | 217-219 | - | 6-8 | - |
Event ids are per file; never reuse a number across files. `EnableBumper/DisableBumper` are enter/exit notify events of
`Weap_Readied_State` / `Magic_Behavior_State` (master). `JumpLandEnd` appears as wildcard trigger and as enter/exit notify. `MTState` is
sent on entering `MT_Behavior_State`.

---------------------------------------------------------------------------------------------------------------------

## 5. Incidental findings in the current modlist (useful when debugging)
- `hkxcheck --character DefaultMale.xml` on the current `1hm_behavior` output reports **6 clips** (`1HM_Idle`, `1HM_RunForward`,
  `1HM_TurnLeft60/180`, `1HM_TurnRight60/180`) whose `animationName` is `Animations\male\1HM_*.hkx`, which are **not in the DefaultMale
  `animationNames`** (7 for DefaultFemale). They come from "Turning Animation Won't Affect UpperBody" (`turn`, node `#4068`) ("Some Nemesis
  defaults for Pandora" ships the same patch). Vanilla SE is consistent (0 errors), so this is a modlist-introduced mismatch; whether the
  game tolerates it was not tested.
- Engine.log failures in the last run (2026-10-18 19:37): 1 Assorted Behavior Fixes (firstperson 0_master quad values), 2 Extra Drawing
  Animations (female weapequip boneWeights), 22 Flinching Animations (`dogproject~wolfbehavior`: its Nemesis ids do not match the shared canine graph), 1 TUDM + 1 Turning (female 1hm `#0426/modifiers` text replace; probably the double application noted in 2.2).
  Nothing from `wbsp`.
- 40 `FNIS Parser ... FAILED` lines are normal (no FNIS list for that folder).

---------------------------------------------------------------------------------------------------------------------

## 6. Source map (what to read for what)
| topic | file (under `PBE/`) |
|---|---|
| op names, path attr, new-node tracking | `Pandora.Skyrim/Format/Pandora/PandoraParser.cs` |
| mod folder, patch file -> packfile | `Pandora.Skyrim/Format/Pandora/PandoraAssembler.cs`, `Projects/ProjectManager.cs`, `Projects/Project.cs` |
| change classes / apply order | `Pandora.Skyrim/Hkx/Changes/*.cs`, `Hkx/Packfile/PackFileDispatcher.cs` |
| path mapping, append/insert/replace/remove on XML | `XML.Cake.NET/XML Cake/Linq/XMapElement.cs`, `Hkx/Packfile/PackFileEditor.cs` |
| macros, dedupe | `Pandora.Skyrim/Hkx/Packfile/PackFileValidator.cs` |
| graph tables, priority nodes | `Hkx/Packfile/PackFileGraph.cs`; character: `PackFileCharacter.cs` |
| export | `Pandora.Skyrim/Patch/IO/PackFileExporter.cs`, `BasePackFileExporter.cs` |
| FNIS lists | `Pandora.Skyrim/Format/FNIS/FNISParser.cs`, `FNISAnimationList.cs`, `BasicAnimation.cs` |
| Nemesis format, animdata | `Pandora.Skyrim/Format/Nemesis/NemesisAssembler.cs`, `NemesisParser.cs`, `AnimData/*` |
| mod discovery, priorities | `Pandora.Core/Mods/*`, `Pandora.Core/Patch/Mod/PandoraModInfo.cs` |
| log | `Pandora.Core/Logging/NLogger/*` |
| native plugins | `Pandora.Core/Patch/Plugins/PluginLoader.cs`, `Pandora.Example.Plugin/` |
| xml -> object deserialisation (null on unknown ref) | `HKX2-Enhanced-Library/HKX2/HavokXmlDeserializer.cs` |

## 7. Not verified (do before relying)
- That a brand-new Pandora mod code appears inactive and what the GUI shows (read from code only).
- Appending pointers to a `numelements="0"` list (read from code only).
- The behaviour of unresolved `#name` pointers in a *real* run (code says null; no run was done because that would write the Pandora output mod).
- Whether the game accepts hkxc-written hkx; hkxc is used strictly as a reader here.

## 8. Tools in this repo
```
python tools\dump_pandora_out.py --vanilla --check            # regenerate _work\pandora_out_xml (+ vanilla Nemesis-id dumps) and run hkxcheck on all
python tools\pandora_log.py --mod "Reactive"                  # grouped Engine.log problems for one mod
python tools\hkxcheck.py X.xml                                # reference/range/stateId checks, exit 1 on error
python tools\hkxcheck.py X.xml --summary RootBehaviorGraph    # states, transitions (event name, target, effect duration/flags)
python tools\hkxcheck.py X.xml --find-event RollStart         # who uses an event
python tools\hkxcheck.py X.xml --tree "#5085" --depth 8       # generator subtree with clips, triggers, expressions, bindings
python tools\hkxcheck.py X.xml --character DefaultMale.xml --require-event RDF_Dodge --require-var RDF_Id
python tools\test_hkxcheck.py                                 # self-test (synthetic XML, mutation cases)
```

## 9. First-person graph (added for RDF)
See BEHAVIOR_INTEGRATION.md section 6.1 for the map: project `firstperson`, patch file `firstperson~0_master.xml`, vanilla Nemesis ids
(`#0095/#0096/#0097` tables, `RootBehaviorGraph #0167`, wildcard `#0168`), same stateIds as 3P, 901-name FirstPerson character that already
holds every weapon-idle clip, and TUDM's 1P dodge states (25/26 in the 1P 1hm/mt/magic graphs). `tools/dump_pandora_out.py` does not dump
the 1P files; `tools/validate_rdf.py` does (`fp_*.xml` under `_work/pandora_test_out/<label>_xml`), and the vanilla 1P dump used for the ids is
`_work/fp_nem_xml` (hkxcmd on `cached_behaviors\_1stperson`).
