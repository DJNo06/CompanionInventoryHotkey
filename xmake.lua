-- include subprojects
includes("lib/commonlibf4")

-- set project constants
set_project("CompanionInventoryHotkey")
set_version("1.1.1")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- version defines (safe for MSVC)
add_defines(
    "CIH_VER_MAJOR=1",
    "CIH_VER_MINOR=1",
    "CIH_VER_PATCH=1"
)

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")
add_rules("win.sdk.resource")

-- define target
target("CompanionInventoryHotkey")

    add_rules("commonlibf4.plugin", {
        name = "CompanionInventoryHotkey",
        author = "DJNo06000",
        description = "Companion Inventory Hotkey Plugin"
    })

    -- rename output dll
    set_filename("CompanionInventoryHotkey.dll")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- auto deploy after build (releasedbg only)
    after_build(function (target)
        if is_mode("releasedbg") then
            local dst = "C:/Program Files (x86)/Steam/steamapps/common/Fallout 4/Data/F4SE/Plugins/"
            os.cp(target:targetfile(), dst)
            print("✔ Deployed to Fallout 4 Plugins folder")
        end
    end)