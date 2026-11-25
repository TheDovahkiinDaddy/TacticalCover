-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("CoverSF")
set_version("0.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- define targets
target("CoverSF")
    add_rules("commonlibsf.plugin", {
        name = "CoverSF",
        author = "CoverSF contributors", -- placeholder: set the real author before release
        description = "Third-person cover system for Starfield"
    })

    -- deployment: the plugin rule installs after every build. When XSE_SF_MODS_PATH is set it targets
    -- <mods>\CoverSF\SFSE\Plugins (our dev setup); when only XSE_SF_GAME_PATH is set, <game>\Data.
    -- This fallback keeps a build with neither variable from installing outside this folder.
    set_installdir(path.join(os.projectdir(), "build", "install"))

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- development INI, installed next to the DLL
    add_installfiles("data/(SFSE/Plugins/CoverSF.ini)")

-- the public release (REFERENCE 24): the same sources as CoverSF, built as TacticalCover.dll, SFSE plugin name "Tactical Cover", version 1.0.0.
-- COVER_RELEASE gives it m4b13's settings as compiled-in defaults (src/ReleaseDefaults.inc), refuses the rig and debug keys and names the INI,
-- the log and the learned file after the DLL. Not built by default and never installed by a build:
--   xmake build TacticalCover      (then tools/make_release.py packs build/release_1.0.0_candidate)
target("TacticalCover")
    set_default(false)
    set_version("1.0.0")
    add_rules("commonlibsf.plugin", {
        name = "Tactical Cover",
        author = "Dovahkiin Daddy",
        description = "Gears of War style third-person cover for Starfield"
    })
    set_values("commonlib.plugin.install", false)
    set_installdir(path.join(os.projectdir(), "build", "install_release"))
    add_defines("COVER_RELEASE")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

-- engine-free tests for src/Core (PLAN.md rule 10). Not built by default:
--   xmake build CoverSF_tests ; xmake run CoverSF_tests
target("CoverSF_tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/*.cpp", "src/Core/*.cpp", "src/ConfigParse.cpp")
    add_includedirs("src")

-- the configuration check (REFERENCE 24): prints the effective settings of an INI, as the development or the release build would read it.
-- Used by tools/compare_config.py. Not built by default.
target("CoverSF_configcheck")
    set_kind("binary")
    set_default(false)
    add_files("tools/config_check.cpp", "src/ConfigParse.cpp")
    add_includedirs("src")
