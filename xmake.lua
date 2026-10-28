set_xmakever("3.0.8")

includes("S:/Programming/CommonLibSSE-NG")

set_project("ReactiveDodgeFramework")
set_version("0.3.0")

add_rules("mode.debug", "mode.releasedbg")
set_defaultplat("windows")
set_defaultarchs("windows|x64")
set_allowedplats("windows")
set_allowedarchs("x64")
set_defaultmode("releasedbg")
set_languages("c++latest")

set_warnings("allextra")

set_policy("build.fence", true)
set_policy("check.auto_ignore_flags", false)

-- CastOn rule files (JSON): glaze reflects the rule structs and reports errors by return value, no exceptions.
add_requires("glaze 7.0.2")

-- Skyrim SE 1.5.97 only.
set_config("skyrim_vr", false)
set_config("skyrim_ae", false)
set_config("skyrim_se", true)
set_config("skse_xbyak", true)

local deploy_dir = "F:/STB/Skyrim/[STB] Mod Organizer/mods/AI - Reactive Dodge Framework/SKSE/Plugins"

target("ReactiveDodgeFramework")
    add_deps("commonlibsse-ng")
    add_packages("glaze")
    add_rules("commonlibsse-ng.plugin", {
        name = "ReactiveDodgeFramework",
        author = "newrite",
        description = "Reactive Dodge Framework: buffered, steerable, animation-driven dodge for Skyrim SE 1.5.97"
    })

    add_includedirs("src")
    -- FLICK (FUCK.dll) API header and the ImGui headers of the revision it is built with (types only)
    add_includedirs("src/third_party/imgui", "src/third_party/fuck")
    set_pcxxheader("src/game/pch.h")
    add_headerfiles("src/**.h")
    add_files("src/main.cpp", "src/game/**.cpp", "src/core/**.cpp")

    after_build(function (target)
        os.mkdir(deploy_dir)
        os.cp(target:targetfile(), deploy_dir)
        local pdb = path.join(path.directory(target:targetfile()), target:basename() .. ".pdb")
        if os.isfile(pdb) then
            os.cp(pdb, deploy_dir)
        end
        -- menu translations (ours, always refreshed): Data/Interface/Translations/ReactiveDodgeFramework_<LANG>.txt
        local translations = path.join(deploy_dir, "..", "..", "Interface", "Translations")
        os.mkdir(translations)
        os.cp("dist/Interface/Translations/*.txt", translations)
        -- Papyrus API (ReactiveDodge.Dodge), compiled from dist/Scripts/Source with S:/SkyrimPapyrus/papyrus.exe
        local scripts = path.join(deploy_dir, "..", "..", "Scripts")
        os.mkdir(path.join(scripts, "Source"))
        os.cp("dist/Scripts/*.pex", scripts)
        os.cp("dist/Scripts/Source/*.psc", path.join(scripts, "Source"))
        -- CastOn rule examples (all disabled): SKSE/Plugins/ReactiveDodgeFramework/CastOn/
        local caston = path.join(deploy_dir, "ReactiveDodgeFramework", "CastOn")
        os.mkdir(caston)
        os.cp("dist/CastOn/*.json", caston)
        local ini = path.join(deploy_dir, "ReactiveDodgeFramework.ini")
        if not os.isfile(ini) then
            os.cp("dist/ReactiveDodgeFramework.ini", ini)
        end
    end)

-- Engine-free core + unit tests: no CommonLib.
target("rdf_tests")
    set_kind("binary")
    set_rundir("$(projectdir)")  -- tests read dist/ files by their repo path
    add_packages("glaze", "xbyak")
    add_includedirs("src", "tests")
    add_headerfiles("src/core/**.h", "tests/**.h")
    add_files("src/core/**.cpp", "tests/**.cpp")
