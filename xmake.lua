set_xmakever("3.0.8")

includes("S:/Programming/CommonLibSSE-NG")

set_project("ReactiveDodgeFramework")
set_version("0.1.0")

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

-- Skyrim SE 1.5.97 only.
set_config("skyrim_vr", false)
set_config("skyrim_ae", false)
set_config("skyrim_se", true)
set_config("skse_xbyak", true)

local deploy_dir = "F:/STB/Skyrim/[STB] Mod Organizer/mods/AI - Reactive Dodge Framework/SKSE/Plugins"

target("ReactiveDodgeFramework")
    add_deps("commonlibsse-ng")
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
        local ini = path.join(deploy_dir, "ReactiveDodgeFramework.ini")
        if not os.isfile(ini) then
            os.cp("dist/ReactiveDodgeFramework.ini", ini)
        end
    end)

-- Engine-free core + unit tests: no CommonLib.
target("rdf_tests")
    set_kind("binary")
    add_includedirs("src", "tests")
    add_headerfiles("src/core/**.h", "tests/**.h")
    add_files("src/core/**.cpp", "tests/**.cpp")
