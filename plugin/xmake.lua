-- ============================================================================
--  Starfield Always Scan - SFSE plugin (SAS_AlwaysScan.dll)
--
--  构建：
--      xmake f -y -p windows -a x64 -m releasedbg --vs=2022
--      xmake build SAS_AlwaysScan
--  （正常情况下不用手动跑，直接 `pwsh tools\build-sas.ps1`）
-- ============================================================================
set_xmakever("3.0.0")

set_project("SAS_AlwaysScan")
set_version("4.6.0")
set_arch("x64")
set_languages("c++23")
set_encodings("utf-8")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")

-- 本地 commonlibsf 源码（提供 commonlibsf 静态库目标与 commonlibsf.plugin 规则）
local commonlibsf_dir = path.join(os.projectdir(), "..", "tools", "commonlibsf-main")
includes(commonlibsf_dir)

target("SAS_AlwaysScan", function()
    set_default(true)
    set_version("4.6.0")
    set_license("GPL-3.0-or-later")

    add_rules("commonlibsf.plugin", {
        name = "Starfield Always Scan",
        author = "SAS",
        description = "Scanner-style object highlighting without equipping the hand scanner",
        xse_minimum = "0.2.21"
    })

    add_files("src/**.cpp")
    add_includedirs("src")
    set_pcxxheader("src/PCH.h")
end)
