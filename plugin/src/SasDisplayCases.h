#pragma once
// ============================================================================
//  自动生成，请勿手改 —— 生成器：tools/re/gen_display_cases.py
//
//  「展示柜（Display Case）」容器白名单：带 BGSDisplayCase 组件（DCSD）的 CONT。
//  这类容器的内容**只在搜刮界面打开期间**以 kTemporary（fl=0x20）条目投影进
//  inventoryList，关闭时读到 size=0 —— 判空规则对它们不适用（v4.9 起跳过）。
//  完整背景见 AlwaysScan.cpp 常量区「v4.9 展示柜」长注释。
//
//  数据来源：Starfield.esm（游戏 1.16.244.0），707 个 CONT 中命中 118 个。
//  重新生成（游戏大版本更新后可跑一次，产物需入库）：
//      python tools/re/gen_display_cases.py <Starfield.esm> plugin/src/SasDisplayCases.h
// ============================================================================

#include <cstddef>
#include <cstdint>

namespace SAS::DisplayCases
{
	// 升序排列（运行期用 std::binary_search 查询）。
	inline constexpr std::uint32_t kBaseIDs[] = {
		0x00003243u,    // Loot_Display_WeaponRack02_Rare_100YearOld
		0x00003244u,    // Loot_Display_WeaponRack01_Common_100YearOld
		0x000052A3u,    // RI07_InternalNeuroampPrototypeCase
		0x000059FBu,    // Loot_Display_WeaponRack02_Common_100YearOld
		0x00005D22u,    // Loot_Display_WeaponRack03_Rare_100YearOld
		0x00005D23u,    // Loot_Display_WeaponsCase_Heavy_Common_100YearOld
		0x00005D27u,    // Loot_Display_WeaponsCase_Heavy_Rare_100YearOld
		0x00005D28u,    // Loot_Display_WeaponsCase_Pistols_Common_100YearOld
		0x00005D2Cu,    // Loot_Display_WeaponsCase_Rifles_Common_100YearOld
		0x00005D2Du,    // Loot_Display_WeaponsCase_Rifles_Rare_100YearOld
		0x0000CC18u,    // 
		0x00019121u,    // Loot_Display_DataSlateKeycard_Multi01_Linked
		0x00019122u,    // Loot_Display_DataSlateKeycard_Single01_Linked
		0x00022469u,    // Loot_Display_KoreKinetics_WeaponRack01
		0x0002246Cu,    // Loot_Display_KoreKinetics_WeaponRack02
		0x00024B01u,    // LC030_Armory_WeaponsCase_Heavy
		0x0002B39Au,    // UC03_Loot_Display_WeaponsCase_Pistols_Legendary
		0x0002B3ACu,    // SubsectionSeven_Loot_Display_WeaponsCase_Pistols_Legendary
		0x0004C085u,    // Loot_Display_WeaponsCase_Rifles_Citizen_UC
		0x0004C086u,    // Loot_Display_WeaponsCase_Pistols_Citizen_UC
		0x0005659Au,    // Loot_Display_WeaponsCase_Heavy_Citizen_UC
		0x0005659Bu,    // Loot_Display_WeaponRack02_Citizen_UC
		0x00057C1Fu,    // Loot_Display_ArboronWeaponRackPanel01
		0x00057C20u,    // Loot_Display_ArboronWeaponRackPanel02
		0x00066520u,    // Loot_Display_WeaponsCase_Rifles_Arboron
		0x0008E851u,    // VS_Loot_Display_WeaponRack01
		0x000B9B80u,    // Loot_Display_WeaponRack02_Common_200YearOldIsolated
		0x000B9BACu,    // Ship_Loot_Display_WeaponRack03_25_ECSConstant
		0x000B9C47u,    // Loot_Display_WeaponRack03_Rare_200YearOldIsolated
		0x000C66BEu,    // Loot_Display_Backpackstand_02_Table_Boostpack_Common
		0x000C66BFu,    // Loot_Display_Backpackstand_01_Standing_Boostpack_Common
		0x000C66C0u,    // Loot_Display_HelmetCase_Rare
		0x000C66C1u,    // Loot_Display_Helmetstand_Rare
		0x000C66C2u,    // Loot_Display_Backpackstand_01_Standing_Any_Rare
		0x000C66C3u,    // Loot_Display_Backpackstand_01_Standing_Boostpack_Rare
		0x000C66C4u,    // Loot_Display_Backpackstand_02_Table_Any_Rare
		0x000C66C5u,    // Loot_Display_Backpackstand_02_Table_Boostpack_Rare
		0x000C66C6u,    // Loot_Display_Backpackstand_03_Wall_Any_Rare
		0x000C66C7u,    // Loot_Display_Backpackstand_03_Wall_Boostpack_Rare
		0x000C66C9u,    // Loot_Display_WeaponRack01_Rare
		0x000C66CAu,    // Loot_Display_WeaponRack02_Rare
		0x000C66CBu,    // Loot_Display_WeaponsCase_Heavy_Rare
		0x000C66CCu,    // Loot_Display_WeaponsCase_Pistols_Rare
		0x000C66CDu,    // Loot_Display_WeaponsCase_Rifles_Rare
		0x000CC43Bu,    // Loot_Display_Backpackstand_02_Table_Citizen_Rare
		0x000DDD5Fu,    // Loot_Display_Helmetstand_Citizen
		0x000FAEB7u,    // Outpost_Display_DataSlate_Multi01
		0x000FAEB8u,    // Outpost_Display_DataSlate_Single01
		0x000FAEB9u,    // Outpost_Display_WeaponRack01
		0x000FAEBAu,    // Outpost_Display_WeaponRack02
		0x000FAEBBu,    // Outpost_Display_WeaponRack03
		0x000FAEBCu,    // Outpost_Display_HelmetCase01
		0x0011A3AEu,    // Outpost_Display_DisplayCaseTall01
		0x0013B00Eu,    // UC08_Loot_Display_WeaponsCase_Heavy_Legendary
		0x0017DE60u,    // Loot_Display_Backpackstand_03_Wall_Boostpack_Common
		0x0018075Cu,    // Loot_Display_Helmetstand_EMPTY
		0x0018075Du,    // Loot_Display_HelmetCase_EMPTY
		0x0018075Eu,    // Loot_Display_Backpackstand_03_Wall_EMPTY
		0x0018075Fu,    // Loot_Display_Backpackstand_02_Table_EMPTY
		0x00180760u,    // Loot_Display_Backpackstand_01_Standing_EMPTY
		0x00182952u,    // Loot_Display_Backpackstand_01_Standing_Boostpack_Rare_200YearOldIsolated
		0x00182956u,    // Loot_Display_Backpackstand_03_Wall_Any_Common_200YearOldIsolated
		0x00182957u,    // Loot_Display_Backpackstand_03_Wall_Boostpack_Common_200YearOldIsolated
		0x00183D7Du,    // Loot_Display_WeaponsCase_Heavy_EMPTY
		0x001870DBu,    // LC003_DisplayCase01
		0x00188D28u,    // Loot_Display_WeaponsCase_Heavy_Common
		0x0018E492u,    // Loot_Display_Mannequinheaddisplay01
		0x0018E494u,    // Loot_Display_Helmetstand_Common
		0x0018E5BCu,    // Loot_Display_Backpackstand_03_Wall_Any_Common
		0x0018E5BDu,    // Loot_Display_Backpackstand_02_Table_Any_Common
		0x0018E5BEu,    // Loot_Display_Backpackstand_01_Standing_Any_Common
		0x00192DFEu,    // Loot_Display_HelmetCase_Common
		0x001A23DDu,    // Loot_Display_WeaponsCase_Rifles_EMPTY
		0x001A23DEu,    // Loot_Display_WeaponsCase_Pistols_EMPTY
		0x001A23DFu,    // Loot_Display_WeaponRack03_EMPTY
		0x001A23E0u,    // Loot_Display_WeaponRack02_EMPTY
		0x001A23E1u,    // Loot_Display_WeaponRack01_EMPTY
		0x001B1DEEu,    // LC003_WeaponsCase_Rifles_Equinox
		0x001C15F2u,    // Loot_Display_WeaponRack03_Rare
		0x001C3849u,    // Loot_Display_WeaponRack02_Common
		0x001C384Bu,    // Loot_Display_WeaponRack01_Common
		0x001F63EAu,    // WeaponsCase_RiflesOpen01
		0x00232439u,    // RI07_InternalNeuroampCase
		0x00243BFFu,    // Loot_Display_DataSlateKeycard_Single01
		0x00243C00u,    // Loot_Display_DataSlateKeycard_Multi01
		0x00244894u,    // Loot_Display_SwordCase_RI05
		0x00246224u,    // Loot_Display_WeaponsCase_Rifles_Common
		0x00246226u,    // Loot_Display_WeaponsCase_Pistols_Common
		0x002BBDE3u,    // Ship_Loot_Display_WeaponsCase_Heavy01_25
		0x002BBDE5u,    // Ship_Loot_Display_WeaponsCase_Pistols01_25
		0x002BBDE7u,    // Ship_Loot_Display_WeaponsCase_Rifles01_25
		0x002BBDF1u,    // Ship_Loot_Display_WeaponRack02_25
		0x002BBDF2u,    // Ship_Loot_Display_WeaponRack01_25
		0x002BBDF4u,    // Ship_Loot_Display_WeaponRack03_25
		0x002BBDF6u,    // Ship_Loot_Display_HelmetCase01_25
		0x002BBDF8u,    // Ship_Loot_Display_Backpackstand_02_Table_25
		0x002BBDF9u,    // Ship_Loot_Display_Helmetstand01_25
		0x002BBDFAu,    // Ship_Loot_Display_Backpackstand_03_Wall_25
		0x002BBDFBu,    // Ship_Loot_Display_Backpackstand_01_Standing_25
		0x002BBDFDu,    // Ship_Loot_Display_WeaponsCase_RiflesOpen01_25
		0x002BD9B9u,    // LC001_Loot_Display_WeaponsCase_EonPistol
		0x00355B14u,    // Outpost_Display_DisplayCaseTall02_End01a
		0x00355B15u,    // Outpost_Display_DisplayCaseTall02_End01b
		0x00355B16u,    // Outpost_Display_DisplayCaseTall02_End02a
		0x00355B17u,    // Outpost_Display_DisplayCaseTall02_End02b
		0x00355B19u,    // Outpost_Display_DisplayCaseTall02_Str01a
		0x00355B1Au,    // Outpost_Display_DisplayCaseTall02_Str01b
		0x00363CC4u,    // Outpost_Display_Backpackstand_01_Standing
		0x00363CC5u,    // Outpost_Display_Backpackstand_02_Table
		0x00363CC6u,    // Outpost_Display_Backpackstand_03_Wall
		0x00363CC7u,    // Outpost_Display_Fancy01
		0x00363CC8u,    // Outpost_Display_HelmetStand01
		0x00363CC9u,    // Outpost_Display_ShelfMedMod05
		0x00367000u,    // Outpost_Display_ArmisticeDisplay_01
		0x00367001u,    // Outpost_Display_Weapon_Wall_Hexagon01
		0x00367002u,    // Outpost_Display_Weapon_Wall_Trapezoid01
		0x00367003u,    // Outpost_Display_Weapon_Wall_Triangle01
		0x0038AE42u,    // Outpost_Display_WeaponCaseTilted01
	};
	inline constexpr std::size_t kBaseIDCount = sizeof(kBaseIDs) / sizeof(kBaseIDs[0]);
}
