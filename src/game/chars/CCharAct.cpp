#include "../../common/flat_containers/flat_set.hpp"
#include "../../common/resource/CResourceLock.h"
#include "../../common/CException.h"
#include "../../network/CClientIterator.h"
#include "../../network/send.h"
#include "../../sphere/ProfileTask.h"
#include "../clients/CClient.h"
#include "../items/CItem.h"
#include "../items/CItemMultiCustom.h"
#include "../components/CCSpawn.h"
#include "../components/CCPropsChar.h"
#include "../components/CCPropsItemChar.h"
#include "../components/CCPropsItemEquippable.h"
#include "../components/CCPropsItemWeapon.h"
#include "../CContainer.h"
#include "../CServer.h"
#include "../CWorld.h"
#include "../CWorldGameTime.h"
#include "../CWorldMap.h"
#include "../CWorldTickingList.h"
#include "../spheresvr.h"
#include "../triggers.h"
#include "CChar.h"
#include "CCharNPC.h"

// "GONAME", "GOTYPE", "GOCHAR"
// 0 = object name
// 1 = char
// 2 = item type
bool CChar::TeleportToObj( int iType, tchar * pszArgs )
{
	ADDTOCALLSTACK("CChar::TeleportToObj");

	const dword dwTotal = g_World.GetUIDCount();
	dword dwUID = m_Act_UID.GetObjUID() & ~UID_F_ITEM;
	dword dwCount = dwTotal-1;

	int iArg = 0;
	if ( iType )
	{
		if ( pszArgs[0] && iType == 1 )
			dwUID = 0;
		iArg = RES_GET_INDEX( Exp_GetVal( pszArgs ));
	}

	while ( dwCount-- )
	{
		if ( ++dwUID >= dwTotal )
		{
			dwUID = 1;
		}
		CObjBase * pObj = g_World.FindUID(dwUID);
		if ( pObj == nullptr )
			continue;

		switch ( iType )
		{
			case 0:
			{
				MATCH_TYPE match = Str_Match(pszArgs, pObj->GetName());
				if (match != MATCH_VALID)
					continue;
			}
			break;
			case 1:	// char
			{
				if (!pObj->IsChar())
					continue;
				if (iArg-- > 0)
					continue;
			}
			break;
			case 2:	// item type
			{
				if (!pObj->IsItem())
					continue;
				const CItem* pItem = static_cast<CItem*>(pObj);
				if (!pItem->IsType((IT_TYPE)iArg))
					continue;
			}
			break;
			case 3: // char id
			{
				if (!pObj->IsChar())
					continue;
				const CChar* pChar = static_cast<CChar*>(pObj);
				if (pChar->GetID() != iArg)
					continue;
			}
			break;
			case 4:	// item id
			{
				if (!pObj->IsItem())
					continue;
				const CItem* pItem = static_cast<CItem*>(pObj);
				if (pItem->GetID() != iArg)
					continue;
			}
			break;
		}

		const CObjBaseTemplate * pObjTop = pObj->GetTopLevelObj();
		if ( pObjTop == nullptr || pObjTop == this )
			continue;
		if ( pObjTop->IsChar() )
		{
			if (!CanDisturb(static_cast<const CChar*>(pObjTop)))
				continue;
		}

		m_Act_UID = pObj->GetUID();
		Spell_Teleport( pObjTop->GetTopPoint(), true, false );
		return true;
	}
	return false;
}

// GoCli
bool CChar::TeleportToCli( int iType, int iArgs )
{
	ADDTOCALLSTACK("CChar::TeleportToCli");

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != nullptr; pClient = it.next())
	{
		if ( ! iType )
		{
			if ( pClient->GetSocketID() != iArgs )
				continue;
		}
		CChar * pChar = pClient->GetChar();
		if ( pChar == nullptr )
			continue;
		if ( ! CanDisturb( pChar ))
			continue;
		if ( iType )
		{
			if ( iArgs-- )
				continue;
		}
		m_Act_UID = pChar->GetUID();
		Spell_Teleport( pChar->GetTopPoint(), true, false );
		return true;
	}
	return false;
}

void CChar::Jail( CTextConsole * pSrc, bool fSet, int iCell )
{
	ADDTOCALLSTACK("CChar::Jail");

	CScriptTriggerArgs Args( fSet? 1 : 0, (int64)(iCell), (int64)(0));

	if ( fSet )	// set the jailed flag.
	{
		if ( IsTrigUsed(TRIGGER_JAILED) )
		{
			if ( OnTrigger( CTRIG_Jailed, pSrc, &Args ) == TRIGRET_RET_TRUE )
				return;
		}

		if ( m_pPlayer )	// allow setting of this to offline chars.
		{
			CAccount *pAccount = m_pPlayer->GetAccount();
			ASSERT(pAccount != nullptr);

			pAccount->SetPrivFlags( PRIV_JAILED );
			pAccount->m_TagDefs.SetNum("JailCell", iCell, true);
		}
		if ( IsClientActive())
		{
			m_pClient->SetPrivFlags( PRIV_JAILED );
		}

		tchar szJailName[ 128 ];
		if ( iCell )
		{
			snprintf( szJailName, sizeof(szJailName), "jail%d", iCell );
		}
		else
		{
			strcpy( szJailName, "jail" );
		}
		Spell_Teleport( g_Cfg.GetRegionPoint( szJailName ), true, false );
		SysMessageDefault( DEFMSG_MSG_JAILED );
	}
	else	// forgive.
	{
		if ( IsTrigUsed(TRIGGER_JAILED) )
		{
			if ( OnTrigger( CTRIG_Jailed, pSrc, &Args ) == TRIGRET_RET_TRUE )
				return;
		}

		if ( IsClientActive())
		{
			if ( ! m_pClient->IsPriv( PRIV_JAILED ))
				return;
			m_pClient->ClearPrivFlags( PRIV_JAILED );
		}
		if ( m_pPlayer )
		{
			CAccount *pAccount = m_pPlayer->GetAccount();
			ASSERT(pAccount != nullptr);

			pAccount->ClearPrivFlags( PRIV_JAILED );
			if ( pAccount->m_TagDefs.GetKey("JailCell") != nullptr )
				pAccount->m_TagDefs.DeleteKey("JailCell");
		}
		SysMessageDefault( DEFMSG_MSG_FORGIVEN );
	}
}

// A vendor is giving me gold. put it in my pack or other place.
void CChar::AddGoldToPack( int iAmount, CItemContainer * pPack, bool fForceNoStack )
{
	ADDTOCALLSTACK("CChar::AddGoldToPack");
    ASSERT(iAmount > 0);

	if ( pPack == nullptr )
		pPack = GetPackSafe();

	if (iAmount > 25000000)
	{
		iAmount = 25000000;
		g_Log.EventWarn("The amount of the new Gold to create is too big. Limited to 25.000.000.\n");
	}
	word iMax = 0;
	CItem * pGold = nullptr;
	while ( iAmount > 0 )
	{
		pGold = CItem::CreateScript( ITEMID_GOLD_C1, this );
		if (!iMax)
			iMax = pGold->GetMaxAmount();

		word iGoldStack = (word)minimum(iAmount, iMax);
		pGold->SetAmount( iGoldStack );

		pPack->ContentAdd( pGold, fForceNoStack );
		iAmount -= iGoldStack;
	}
	if (pGold)
		Sound( pGold->GetDropSound( pPack ));
	UpdateStatsFlag();
}

// add equipped items.
// check for item already in that layer ?
// NOTE: This could be part of the Load as well so it may not truly be being "equipped" at this time.
// OnTrigger for equip is done by ItemEquip()
void CChar::LayerAdd( CItem * pItem, LAYER_TYPE layer )
{
	ADDTOCALLSTACK("CChar::LayerAdd");

	if ( pItem == nullptr )
		return;
	if ( (pItem->GetParent() == this) && ( pItem->GetEquipLayer() == layer ) )
		return;

	if ( layer == LAYER_DRAGGING )
	{
		pItem->RemoveSelf();	// remove from where i am before add so UNEQUIP effect takes.
		// NOTE: CanEquipLayer may bounce an item . If it stacks with this we are in trouble !
	}

	if ( g_Serv.IsLoading() == false )
	{
		// This takes care of any conflicting items in the slot !
		layer = CanEquipLayer(pItem, layer, nullptr, false);
		if ( layer == LAYER_NONE )
		{
			// we should not allow non-layered stuff to be put here ?
			// Put in pack instead ?
			ItemBounce( pItem );
			return;
		}

		if (!pItem->IsTypeSpellable() && !pItem->m_itSpell.m_spell && !pItem->IsType(IT_WAND))	// can this item have a spell effect ? If so we do not send
		{
			if ((IsTrigUsed(TRIGGER_MEMORYEQUIP)) || (IsTrigUsed(TRIGGER_ITEMMEMORYEQUIP)))
			{
				CScriptTriggerArgs pArgs;
				pArgs.m_iN1 = layer;
				if (pItem->OnTrigger(ITRIG_MemoryEquip, this, &pArgs) == TRIGRET_RET_TRUE)
				{
					pItem->Delete();
					return;
				}
			}
		}
	}

	if ( layer == LAYER_SPECIAL )
	{
		if ( pItem->IsType( IT_EQ_TRADE_WINDOW ))
			layer = LAYER_NONE;
	}

	CContainer::ContentAddPrivate( pItem );
	pItem->SetEquipLayer( layer );

	// update flags etc for having equipped this.
	switch ( layer )
	{
		case LAYER_HAND1:
		case LAYER_HAND2:
			// If weapon
			if ( pItem->IsTypeWeapon())
			{
				m_uidWeapon = pItem->GetUID();
                if (m_pPlayer)
                    m_pPlayer->m_uidWeaponLast = m_uidWeapon;
				if ( Fight_IsActive() )
					Skill_Start(Fight_GetWeaponSkill());	// update char action
			}
			else if ( pItem->IsTypeArmor())
			{
				// Shield of some sort.
				m_defense = (word)(CalcArmorDefense());
				StatFlag_Set( STATF_HASSHIELD );
				UpdateStatsFlag();
			}
			break;
		case LAYER_SHOES:
		case LAYER_PANTS:
		case LAYER_SHIRT:
		case LAYER_HELM:		// 6
		case LAYER_GLOVES:	// 7
		case LAYER_COLLAR:	// 10 = gorget or necklace.
		case LAYER_HALF_APRON:
		case LAYER_CHEST:	// 13 = armor chest
		case LAYER_TUNIC:	// 17 = jester suit
		case LAYER_ARMS:		// 19 = armor
		case LAYER_CAPE:		// 20 = cape
		case LAYER_ROBE:		// 22 = robe over all.
		case LAYER_SKIRT:
		case LAYER_LEGS:
			// If armor or clothing = change in defense rating.
			m_defense = (word)CalcArmorDefense();
			UpdateStatsFlag();
			break;

			// These effects are not magical. (make them spells !)

		case LAYER_FLAG_Criminal:
			StatFlag_Set( STATF_CRIMINAL );
			NotoSave_Update();
            if (m_pClient)
            {
                m_pClient->addBuff(BI_CRIMINALSTATUS, 1153802, 1153828);
            }
			return;
		case LAYER_FLAG_SpiritSpeak:
			StatFlag_Set( STATF_SPIRITSPEAK );
			return;
		case LAYER_FLAG_Stuck:
			StatFlag_Set( STATF_FREEZE );
			if ( IsClientActive() )
				GetClientActive()->addBuff(BI_PARALYZE, 1075827, 1075828, (word)(pItem->GetTimerSAdjusted()));
			break;
		default:
			break;
	}

	if ( layer != LAYER_DRAGGING )
	{
		switch ( pItem->GetType())
		{
			case IT_EQ_SCRIPT:	// pure script.
				break;
			case IT_EQ_MEMORY_OBJ:
			{
				CItemMemory *pMemory = dynamic_cast<CItemMemory *>( pItem );
				if (pMemory != nullptr)
					Memory_UpdateFlags(pMemory);
				break;
			}
			case IT_EQ_HORSE:
				StatFlag_Set(STATF_ONHORSE);
				break;
			case IT_COMM_CRYSTAL:
				StatFlag_Set(STATF_COMM_CRYSTAL);
				break;
			default:
				break;
		}
	}

	pItem->Update();
}

// Unequip the item.
// This may be a delete etc. It can not FAIL !
// Removing 'Equip beneficts' from this item
void CChar::OnRemoveObj( CSObjContRec* pObRec )	// Override this = called when removed from list.
{
	ADDTOCALLSTACK("CChar::OnRemoveObj");
	CItem * pItem = static_cast <CItem*>(pObRec);
	if ( !pItem )
		return;

	LAYER_TYPE layer = pItem->GetEquipLayer();
	if (( IsTrigUsed(TRIGGER_UNEQUIP) ) || ( IsTrigUsed(TRIGGER_ITEMUNEQUIP) ))
	{
		if ( layer != LAYER_DRAGGING && ! g_Serv.IsLoading())
			pItem->OnTrigger( ITRIG_UNEQUIP, this );
	}

	CContainer::OnRemoveObj( pObRec );

	// remove equipped items effects
	switch ( layer )
	{
		case LAYER_HAND1:
		case LAYER_HAND2:	// other hand = shield
			if ( pItem->IsTypeWeapon())
			{
				m_uidWeapon.InitUID();
				if ( Fight_IsActive() )
					Skill_Start(Fight_GetWeaponSkill());	// update char action
			}
			else if ( pItem->IsTypeArmor())
			{
				// Shield
				m_defense = (word)(CalcArmorDefense());
				StatFlag_Clear( STATF_HASSHIELD );
				UpdateStatsFlag();
			}
            if ( g_Cfg.IsSkillFlag(m_Act_SkillCurrent, SKF_GATHER) )
			{
				Skill_Cleanup();
			}
			break;
		case LAYER_SHOES:
		case LAYER_PANTS:
		case LAYER_SHIRT:
		case LAYER_HELM:		// 6
		case LAYER_GLOVES:	// 7
		case LAYER_COLLAR:	// 10 = gorget or necklace.
		case LAYER_CHEST:	// 13 = armor chest
		case LAYER_TUNIC:	// 17 = jester suit
		case LAYER_ARMS:		// 19 = armor
		case LAYER_CAPE:		// 20 = cape
		case LAYER_ROBE:		// 22 = robe over all.
		case LAYER_SKIRT:
		case LAYER_LEGS:
			m_defense = (word)(CalcArmorDefense());
			UpdateStatsFlag();
			break;

		case LAYER_FLAG_Criminal:
			StatFlag_Clear( STATF_CRIMINAL );
			NotoSave_Update();
            if (m_pClient)
            {
                m_pClient->removeBuff(BI_CRIMINALSTATUS);
            }
			break;
		case LAYER_FLAG_SpiritSpeak:
			StatFlag_Clear( STATF_SPIRITSPEAK );
			break;
		case LAYER_FLAG_Stuck:
			StatFlag_Clear( STATF_FREEZE );
			if ( IsClientActive() )
			{
				GetClientActive()->removeBuff(BI_PARALYZE);
				GetClientActive()->addCharMove(this);		// immediately tell the client that now he's able to move (without this, it will be able to move only on next tick update)
			}
			break;
		default:
			break;
	}

    // Items with magic effects.
    if (layer == LAYER_DRAGGING)
        return; // Stop here

    switch (pItem->GetType())
    {
        case IT_COMM_CRYSTAL:
            if (ContentFind(CResourceID(RES_TYPEDEF, IT_COMM_CRYSTAL), 0, 0) == nullptr)
                StatFlag_Clear(STATF_COMM_CRYSTAL);
            break;
        case IT_EQ_HORSE:
            StatFlag_Clear(STATF_ONHORSE);
            break;
        case IT_EQ_MEMORY_OBJ:
        {
            // Clear the associated flags.
            CItemMemory *pMemory = dynamic_cast<CItemMemory*>(pItem);
            if (pMemory != nullptr)
                Memory_UpdateClearTypes(pMemory, 0xFFFF);
            break;
        }
        default:
            break;
    }


    const CBaseBaseDef* pItemBase = pItem->Base_GetDef();

    // Start of CCPropsItemEquippable props
    CCPropsChar *pCCPChar = GetComponentProps<CCPropsChar>();
    CCPropsItemEquippable *pItemCCPItemEquippable = pItem->GetComponentProps<CCPropsItemEquippable>();
    const CCPropsItemEquippable *pItemBaseCCPItemEquippable = pItemBase->GetComponentProps<CCPropsItemEquippable>();

    if (pItemCCPItemEquippable || pItemBaseCCPItemEquippable)
    {
        // Leave the stat bonuses signed, since they can be used also as a malus (negative sign)
        Stat_AddMod(STAT_STR,       - (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSSTR, pItemBaseCCPItemEquippable));
        Stat_AddMod(STAT_DEX,       - (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSDEX, pItemBaseCCPItemEquippable));
        Stat_AddMod(STAT_INT,       - (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSINT, pItemBaseCCPItemEquippable));
        Stat_AddMaxMod(STAT_STR,    - (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSHITSMAX, pItemBaseCCPItemEquippable));
        Stat_AddMaxMod(STAT_DEX,    - (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSSTAMMAX, pItemBaseCCPItemEquippable));
        Stat_AddMaxMod(STAT_INT,    - (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSMANAMAX, pItemBaseCCPItemEquippable));

        ModPropNum(pCCPChar, PROPCH_RESPHYSICAL,  - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESPHYSICAL, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESFIRE,      - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESFIRE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESCOLD,      - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESCOLD, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESPOISON,    - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESPOISON, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESENERGY,    - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESENERGY, pItemBaseCCPItemEquippable));

        ModPropNum(pCCPChar, PROPCH_INCREASEDAM,          - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASEDAM, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASEDEFCHANCE,    - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASEDEFCHANCE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_FASTERCASTING,        - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_FASTERCASTING, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASEHITCHANCE,    - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASEHITCHANCE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASESPELLDAM,     - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASESPELLDAM, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASESWINGSPEED,   - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASESWINGSPEED, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_ENHANCEPOTIONS,       - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_ENHANCEPOTIONS, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_LOWERMANACOST,        - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_LOWERMANACOST, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_LUCK,                 - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_LUCK, pItemBaseCCPItemEquippable));

        ModPropNum(pCCPChar, PROPCH_DAMPHYSICAL,   - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMPHYSICAL, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMFIRE,       - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMFIRE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMCOLD,       - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMCOLD, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMPOISON,     - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMPOISON, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMENERGY,     - pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMENERGY, pItemBaseCCPItemEquippable));

        if (pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_NIGHTSIGHT, pItemBaseCCPItemEquippable))
        {
            StatFlag_Mod(STATF_NIGHTSIGHT, 0);
            if (IsClientActive())
                m_pClient->addLight();
        }

        if (pItem->IsTypeWeapon())
        {
            CItem * pCursedMemory = LayerFind(LAYER_SPELL_Curse_Weapon);	// Remove the cursed state from SPELL_Curse_Weapon.
            if (pCursedMemory)
                pItem->ModPropNum(pItemCCPItemEquippable, PROPIEQUIP_HITLEECHLIFE, - pCursedMemory->m_itSpell.m_spelllevel, pItemBaseCCPItemEquippable);
        }
    }
    // End of CCPropsItemEquippable props

    // If items are magical then remove effect here.
    Spell_Effect_Remove(pItem);
}

// shrunk or died. (or sleeping)
void CChar::DropAll( CItemContainer * pCorpse, uint64 iAttr )
{
	ADDTOCALLSTACK("CChar::DropAll");
	if ( IsStatFlag( STATF_CONJURED ))
		return;	// drop nothing.

	CItemContainer * pPack = GetPack();
	if ( pPack != nullptr )
	{
		if ( pCorpse == nullptr )
		{
			pPack->ContentsDump( GetTopPoint(), iAttr );
		}
		else
		{
			pPack->ContentsTransfer( pCorpse, true );
		}
	}

	// transfer equipped items to corpse or your pack (if newbie).
	UnEquipAllItems( pCorpse );
}

// We morphed, sleeping, died or became a GM.
// Pets can be told to "Drop All"
// drop item that is up in the air as well.
// pDest       = Container to place items in
// fLeaveHands = true to leave items in hands; otherwise, false
void CChar::UnEquipAllItems( CItemContainer * pDest, bool fLeaveHands )
{
	ADDTOCALLSTACK("CChar::UnEquipAllItems");

	if ( IsContainerEmpty())
		return;

	CItemContainer *pPack = GetPackSafe();
	for (CSObjContRec* pObjRec : GetIterationSafeContReverse())
	{
		if (pObjRec->GetParent() != this)
			continue;

		CItem* pItem = static_cast<CItem*>(pObjRec);
		LAYER_TYPE layer = pItem->GetEquipLayer();

		switch ( layer )
		{
			case LAYER_NONE:
				pItem->Delete();	// Get rid of any trades.
				continue;
			case LAYER_FLAG_Poison:
			case LAYER_FLAG_Hallucination:
			case LAYER_FLAG_Potion:
			case LAYER_FLAG_Drunk:
			case LAYER_FLAG_Stuck:
			case LAYER_FLAG_PotionUsed:
				if ( IsStatFlag(STATF_DEAD) )
					pItem->Delete();
				continue;
			case LAYER_PACK:
			case LAYER_HORSE:
				continue;
			case LAYER_HAIR:
			case LAYER_BEARD:
				// Copy hair and beard to corpse.
				if ( pDest && pDest->IsType(IT_CORPSE) )
				{
					CItem *pDupe = CItem::CreateDupeItem(pItem);
					pDest->ContentAdd(pDupe);
					// Equip layer only matters on a corpse.
					pDupe->SetContainedLayer((byte)layer);
				}
				continue;
			case LAYER_DRAGGING:
				layer = LAYER_NONE;
				break;
			case LAYER_HAND1:
			case LAYER_HAND2:
				if ( fLeaveHands )
					continue;
				break;
			default:
				// can't transfer this to corpse.
				if ( !CItemBase::IsVisibleLayer(layer) )
					continue;
				break;
		}

		if ( pDest && !pItem->IsAttr(ATTR_NEWBIE|ATTR_MOVE_NEVER|ATTR_BLESSED|ATTR_INSURED|ATTR_NODROP|ATTR_NOTRADE) )
		{
			// Move item to dest (corpse usually)
			pDest->ContentAdd(pItem);
			if ( pDest->IsType(IT_CORPSE) )
			{
				// Equip layer only matters on a corpse.
				pItem->SetContainedLayer((byte)layer);
			}
		}
		else if ( pPack )
		{
			// Move item to char pack.
			pPack->ContentAdd(pItem);
		}
	}
}

// Show the world that I am picking up or putting down this object.
// NOTE: This makes people disapear.
void CChar::UpdateDrag( CItem * pItem, CObjBase * pCont, CPointMap * pt )
{
	ADDTOCALLSTACK("CChar::UpdateDrag");

	if ( pCont && pCont->GetTopLevelObj() == this )		// moving to my own backpack
		return;
	if ( !pCont && !pt && pItem->GetTopLevelObj() == this )		// doesn't work for ground objects
		return;

	PacketDragAnimation* cmd = new PacketDragAnimation(this, pItem, pCont, pt);
	UpdateCanSee(cmd, m_pClient);
}

void CChar::ObjMessage( lpctstr pMsg, const CObjBase * pSrc ) const
{
	if ( ! IsClientActive())
		return;
	GetClientActive()->addObjMessage( pMsg, pSrc );
}
void CChar::SysMessage( lpctstr pMsg ) const	// Push a message back to the client if there is one.
{
	if ( ! IsClientActive())
		return;
	GetClientActive()->SysMessage( pMsg );
}

// Push status change to all who can see us.
// For Weight, AC, Gold must update all
// Just flag the stats to be updated later if possible.
void CChar::UpdateStatsFlag() const
{
	ADDTOCALLSTACK("CChar::UpdateStatsFlag");
	if ( g_Serv.IsLoading() )
		return;

	if ( IsClientActive() )
		GetClientActive()->addUpdateStatsFlag();
}

// queue updates

void CChar::UpdateHitsFlag()
{
	ADDTOCALLSTACK("CChar::UpdateHitsFlag");
	if ( g_Serv.IsLoading() )
		return;

	m_fStatusUpdate |= SU_UPDATE_HITS;

	if ( IsClientActive() )
		GetClientActive()->addUpdateHitsFlag();
}

void CChar::UpdateModeFlag()
{
	ADDTOCALLSTACK("CChar::UpdateModeFlag");
	if ( g_Serv.IsLoading() )
		return;

	m_fStatusUpdate |= SU_UPDATE_MODE;
}

void CChar::UpdateManaFlag() const
{
	ADDTOCALLSTACK("CChar::UpdateManaFlag");
	if ( g_Serv.IsLoading() )
		return;

	if ( IsClientActive() )
		GetClientActive()->addUpdateManaFlag();
}

void CChar::UpdateStamFlag() const
{
	ADDTOCALLSTACK("CChar::UpdateStamFlag");
	if ( g_Serv.IsLoading() )
		return;

	if ( IsClientActive() )
		GetClientActive()->addUpdateStamFlag();
}

void CChar::UpdateStatVal( STAT_TYPE type, int iChange, ushort uiLimit )
{
	ADDTOCALLSTACK("CChar::UpdateStatVal");
	const int iValPrev = Stat_GetVal(type);
	int iVal = iValPrev + iChange;

	if ( iVal < 0 )
		iVal = 0;
    if (IsSetOF(OF_StatAllowValOverMax))
    {
        if ((iVal >= uiLimit) && (uiLimit != 0) && (iChange >= 0))
            iVal = uiLimit;
    }
    else
    {
        if (uiLimit == 0)
            uiLimit = Stat_GetMaxAdjusted(type);
        if (iVal >= uiLimit)
            iVal = uiLimit;
    }
	if ( iVal == iValPrev )
		return;
	if (iVal > UINT16_MAX)
		iVal = UINT16_MAX;

	Stat_SetVal(type, (ushort)iVal);

	switch ( type )
	{
		case STAT_STR:
			UpdateHitsFlag();
			break;
		case STAT_INT:
			UpdateManaFlag();
			break;
		case STAT_DEX:
			UpdateStamFlag();
			break;
	}
}

// Calculate the action to be used to call UpdateAnimate() with it
ANIM_TYPE CChar::GenerateAnimate( ANIM_TYPE action, bool fTranslate, bool fBackward, byte iFrameDelay, byte iAnimLen )
{
	ADDTOCALLSTACK("CChar::UpdateAnimate");
	UNREFERENCED_PARAMETER(iAnimLen);
	if ( action < 0 || action >= ANIM_QTY )
		return (ANIM_TYPE)-1;

	const CCharBase* pCharDef = Char_GetDef();

	//Begin old client animation behaviour

	if ( fBackward && iFrameDelay )	// backwards and delayed just dont work ! = invis
		iFrameDelay = 0;

	if (fTranslate || IsStatFlag(STATF_ONHORSE))
	{
		CItem * pWeapon = m_uidWeapon.ItemFind();
		if (pWeapon != nullptr && action == ANIM_ATTACK_WEAPON)
		{
			// action depends on weapon type (skill) and 2 Hand type.
			LAYER_TYPE layer = pWeapon->Item_GetDef()->GetEquipLayer();
			switch (pWeapon->GetType())
			{
                default:
			    case IT_WEAPON_MACE_CROOK:	// sheperds crook
			    case IT_WEAPON_MACE_SMITH:	// smith/sledge hammer
			    case IT_WEAPON_MACE_STAFF:
			    case IT_WEAPON_MACE_SHARP:	// war axe can be used to cut/chop trees.
				    action = (layer == LAYER_HAND2) ? ANIM_ATTACK_2H_BASH : ANIM_ATTACK_1H_BASH;
				    break;
			    case IT_WEAPON_SWORD:
			    case IT_WEAPON_AXE:
			    case IT_WEAPON_MACE_PICK:	// pickaxe
				    action = (layer == LAYER_HAND2) ? ANIM_ATTACK_2H_SLASH : ANIM_ATTACK_1H_SLASH;
				    break;
			    case IT_WEAPON_FENCE:
				    action = (layer == LAYER_HAND2) ? ANIM_ATTACK_2H_PIERCE : ANIM_ATTACK_1H_PIERCE;
				    break;
			    case IT_WEAPON_THROWING:
				    action = ANIM_ATTACK_1H_SLASH;
				    break;
			    case IT_WEAPON_BOW:
				    action = ANIM_ATTACK_BOW;
				    break;
			    case IT_WEAPON_XBOW:
				    action = ANIM_ATTACK_XBOW;
				    break;
                case IT_WEAPON_WHIP:
                    action = ANIM_ATTACK_1H_BASH;
                    break;
			}
			// Temporary disabled - it's causing weird animations on some weapons
			/*if ((Calc_GetRandVal(2)) && (pWeapon->GetType() != IT_WEAPON_BOW) && (pWeapon->GetType() != IT_WEAPON_XBOW) && (pWeapon->GetType() != IT_WEAPON_THROWING))
			{
				// add some style to the attacks.
				if (layer == LAYER_HAND2)
					action = static_cast<ANIM_TYPE>(ANIM_ATTACK_2H_BASH + Calc_GetRandVal(3));
				else
					action = static_cast<ANIM_TYPE>(ANIM_ATTACK_1H_SLASH + Calc_GetRandVal(3));
			}*/
		}

		if (IsStatFlag(STATF_ONHORSE))	// on horse back.
		{
			// Horse back anims are dif.
			switch (action)
			{
			case ANIM_WALK_UNARM:
			case ANIM_WALK_ARM:
				return ANIM_HORSE_RIDE_SLOW;
			case ANIM_RUN_UNARM:
			case ANIM_RUN_ARMED:
				return ANIM_HORSE_RIDE_FAST;
			case ANIM_STAND:
				return ANIM_HORSE_STAND;
			case ANIM_FIDGET1:
			case ANIM_FIDGET_YAWN:
				return ANIM_HORSE_SLAP;
			case ANIM_STAND_WAR_1H:
			case ANIM_STAND_WAR_2H:
				return ANIM_HORSE_STAND;
			case ANIM_ATTACK_1H_SLASH:
			case ANIM_ATTACK_1H_PIERCE:
			case ANIM_ATTACK_1H_BASH:
				return ANIM_HORSE_ATTACK;
			case ANIM_ATTACK_2H_BASH:
			case ANIM_ATTACK_2H_SLASH:
			case ANIM_ATTACK_2H_PIERCE:
				return ANIM_HORSE_SLAP;
			case ANIM_WALK_WAR:
				return ANIM_HORSE_RIDE_SLOW;
			case ANIM_CAST_DIR:
				return ANIM_HORSE_ATTACK;
			case ANIM_CAST_AREA:
				return ANIM_HORSE_ATTACK_BOW;
			case ANIM_ATTACK_BOW:
				return ANIM_HORSE_ATTACK_BOW;
			case ANIM_ATTACK_XBOW:
				return ANIM_HORSE_ATTACK_XBOW;
			case ANIM_GET_HIT:
				return ANIM_HORSE_SLAP;
			case ANIM_BLOCK:
				return ANIM_HORSE_SLAP;
			case ANIM_ATTACK_WRESTLE:
				return ANIM_HORSE_ATTACK;
			case ANIM_BOW:
			case ANIM_SALUTE:
			case ANIM_EAT:
				return ANIM_HORSE_ATTACK_XBOW;
			default:
				return ANIM_HORSE_STAND;
			}
		}
		else if (!IsPlayableCharacter())  //( GetDispID() < CREID_MAN ) Possible fix for anims not being displayed above 400
		{
			// Animals have certain anims. Monsters have others.

			if (GetDispID() >= CREID_HORSE_TAN)
			{
				// All animals have all these anims thankfully
				switch (action)
				{
					case ANIM_WALK_UNARM:
					case ANIM_WALK_ARM:
					case ANIM_WALK_WAR:
						return ANIM_ANI_WALK;
					case ANIM_RUN_UNARM:
					case ANIM_RUN_ARMED:
						return ANIM_ANI_RUN;
					case ANIM_STAND:
					case ANIM_STAND_WAR_1H:
					case ANIM_STAND_WAR_2H:

					case ANIM_FIDGET1:
						return ANIM_ANI_FIDGET1;
					case ANIM_FIDGET_YAWN:
						return ANIM_ANI_FIDGET2;
					case ANIM_CAST_DIR:
						return ANIM_ANI_ATTACK1;
					case ANIM_CAST_AREA:
						return ANIM_ANI_EAT;
					case ANIM_GET_HIT:
						return ANIM_ANI_GETHIT;

					case ANIM_ATTACK_1H_SLASH:
					case ANIM_ATTACK_1H_PIERCE:
					case ANIM_ATTACK_1H_BASH:
					case ANIM_ATTACK_2H_BASH:
					case ANIM_ATTACK_2H_SLASH:
					case ANIM_ATTACK_2H_PIERCE:
					case ANIM_ATTACK_BOW:
					case ANIM_ATTACK_XBOW:
					case ANIM_ATTACK_WRESTLE:
						switch (Calc_GetRandVal(2))
						{
							case 0: return ANIM_ANI_ATTACK1; break;
							case 1: return ANIM_ANI_ATTACK2; break;
						}

					case ANIM_DIE_BACK:
						return ANIM_ANI_DIE1;
					case ANIM_DIE_FORWARD:
						return ANIM_ANI_DIE2;
					case ANIM_BLOCK:
					case ANIM_BOW:
					case ANIM_SALUTE:
						return ANIM_ANI_SLEEP;
					case ANIM_EAT:
						return ANIM_ANI_EAT;
					default:
						break;
				}

				ASSERT(action < ANIM_MASK_MAX);
				while (action != ANIM_WALK_UNARM && !(pCharDef->m_Anims & (1ULL << action)))
				{
					// This anim is not supported. Try to use one that is.
					switch (action)
					{
						case ANIM_ANI_SLEEP:	// All have this.
							return ANIM_ANI_EAT;
						default:
							return ANIM_WALK_UNARM;
					}
				}
			}
			else
			{
				// Monsters don't have all the anims.

				switch (action)
				{
					case ANIM_CAST_DIR:
						return ANIM_MON_Stomp;
					case ANIM_CAST_AREA:
						return ANIM_MON_PILLAGE;
					case ANIM_DIE_BACK:
						return ANIM_MON_DIE1;
					case ANIM_DIE_FORWARD:
						return ANIM_MON_DIE2;
					case ANIM_GET_HIT:
						switch (Calc_GetRandVal(3))
						{
							case 0: return ANIM_MON_GETHIT; break;
							case 1: return ANIM_MON_BlockRight; break;
							case 2: return ANIM_MON_BlockLeft; break;
						}
						break;
					case ANIM_ATTACK_1H_SLASH:
					case ANIM_ATTACK_1H_PIERCE:
					case ANIM_ATTACK_1H_BASH:
					case ANIM_ATTACK_2H_BASH:
					case ANIM_ATTACK_2H_PIERCE:
					case ANIM_ATTACK_2H_SLASH:
					case ANIM_ATTACK_BOW:
					case ANIM_ATTACK_XBOW:
					case ANIM_ATTACK_WRESTLE:
						switch (Calc_GetRandVal(3))
						{
							case 0: return ANIM_MON_ATTACK1; break;
							case 1: return ANIM_MON_ATTACK2; break;
							case 2: return ANIM_MON_ATTACK3; break;
						}
					default:
						return ANIM_WALK_UNARM;
				}
				// NOTE: Available actions depend HEAVILY on creature type !
				// ??? Monsters don't have all anims in common !
				// translate these !
				ASSERT(action < ANIM_MASK_MAX);
				while (action != ANIM_WALK_UNARM && !(pCharDef->m_Anims & (1ULL << action)))
				{
					// This anim is not supported. Try to use one that is.
					switch (action)
					{
					case ANIM_MON_ATTACK1:	// All have this.
						DEBUG_ERR(("Anim 0%x This is wrong! Invalid SCP file data.\n", GetDispID()));
						return ANIM_WALK_UNARM;

					case ANIM_MON_ATTACK2:	// Dolphins, Eagles don't have this.
					case ANIM_MON_ATTACK3:
						return ANIM_MON_ATTACK1;	// ALL creatures have at least this attack.
					case ANIM_MON_Cast2:	// Trolls, Spiders, many others don't have this.
						return ANIM_MON_BlockRight;	// Birds don't have this !
					case ANIM_MON_BlockRight:
						return ANIM_MON_BlockLeft;
					case ANIM_MON_BlockLeft:
						return ANIM_MON_GETHIT;
					case ANIM_MON_GETHIT:
						if (pCharDef->m_Anims & (1 << ANIM_MON_Cast2))
							return ANIM_MON_Cast2;
						else
							return ANIM_WALK_UNARM;

					case ANIM_MON_Stomp:
						return ANIM_MON_PILLAGE;
					case ANIM_MON_PILLAGE:
						return ANIM_MON_ATTACK3;
					case ANIM_MON_AttackBow:
					case ANIM_MON_AttackXBow:
						return ANIM_MON_ATTACK3;
					case ANIM_MON_AttackThrow:
						return ANIM_MON_AttackXBow;

					default:
						DEBUG_ERR(("Anim Unsupported 0%x for 0%x\n", action, GetDispID()));
						return ANIM_WALK_UNARM;
					}
				}
			}
		}
	}

	return action;
}

// NPC or character does a certain Animate
// Translate the animation based on creature type.
// ARGS:
//   fBackward = make the anim go in reverse.
//   iFrameDelay = in seconds (approx), 0=fastest, 1=slower
bool CChar::UpdateAnimate(ANIM_TYPE action, bool fTranslate, bool fBackward , byte iFrameDelay , byte iAnimLen)
{
	ADDTOCALLSTACK("CChar::UpdateAnimate");
	if (action < 0 || action >= ANIM_QTY)
		return false;

	ANIM_TYPE_NEW subaction = (ANIM_TYPE_NEW)(-1);
	byte variation = 0;		//Seems to have some effect for humans/elfs vs gargoyles
	if (fTranslate)
		action = GenerateAnimate(action, true, fBackward);
	ANIM_TYPE_NEW action1 = (ANIM_TYPE_NEW)(action);

	if (IsPlayableCharacter())		//Perform these checks only for Gargoyles or in Enhanced Client
	{
		CItem * pWeapon = m_uidWeapon.ItemFind();
		if (pWeapon &&
            ( (action == ANIM_ATTACK_WEAPON) || (action == ANIM_ATTACK_BOW) || (action == ANIM_ATTACK_XBOW) || (action == ANIM_HORSE_SLAP) ||
              (action == ANIM_HORSE_ATTACK) || (action == ANIM_HORSE_ATTACK_BOW) || (action == ANIM_HORSE_ATTACK_XBOW)
            ) )
		{
			if (!IsGargoyle())		//Set variation to 1 for non gargoyle characters (Humans and Elfs using EC) in all fighting animations.
				variation = 1;
			// action depends on weapon type (skill) and 2 Hand type.
			LAYER_TYPE layer = pWeapon->Item_GetDef()->GetEquipLayer();
			action1 = NANIM_ATTACK; //Should be NANIM_ATTACK;
			switch (pWeapon->GetType())
			{
				case IT_WEAPON_MACE_CROOK:
				case IT_WEAPON_MACE_PICK:
				case IT_WEAPON_MACE_SMITH:	// Can be used for smithing ?
				case IT_WEAPON_MACE_STAFF:
				case IT_WEAPON_MACE_SHARP:	// war axe can be used to cut/chop trees.
					subaction = (layer == LAYER_HAND2) ? NANIM_ATTACK_2H_BASH : NANIM_ATTACK_1H_BASH;
					break;
				case IT_WEAPON_SWORD:
				case IT_WEAPON_AXE:
					subaction = (layer == LAYER_HAND2) ? NANIM_ATTACK_2H_PIERCE : NANIM_ATTACK_1H_PIERCE;
					break;
				case IT_WEAPON_FENCE:
					subaction = (layer == LAYER_HAND2) ? NANIM_ATTACK_2H_SLASH : NANIM_ATTACK_1H_SLASH;
					break;
				case IT_WEAPON_THROWING:
					subaction = NANIM_ATTACK_THROWING;
					break;
				case IT_WEAPON_BOW:
					subaction = NANIM_ATTACK_BOW;
					break;
				case IT_WEAPON_XBOW:
					subaction = NANIM_ATTACK_CROSSBOW;
					break;
                case IT_WEAPON_WHIP:
                    subaction = NANIM_ATTACK_1H_BASH;
                    break;
				default:
					break;
			}
		}
		else
		{
			switch (action)
			{
                case ANIM_HORSE_ATTACK:
					action1 = NANIM_ATTACK;
					subaction = NANIM_ATTACK_2H_BASH;
					break;
				case ANIM_ATTACK_1H_PIERCE:
                case ANIM_ATTACK_1H_SLASH:
					action1 = NANIM_ATTACK;
					subaction = NANIM_ATTACK_1H_SLASH;
					break;
				case ANIM_ATTACK_1H_BASH:
					action1 = NANIM_ATTACK;
					subaction = NANIM_ATTACK_1H_PIERCE;
					break;
                case ANIM_HORSE_SLAP:
				case ANIM_ATTACK_2H_PIERCE:
					action1 = NANIM_ATTACK;
					subaction = NANIM_ATTACK_2H_SLASH;
					break;
				case ANIM_ATTACK_2H_SLASH:
					action1 = NANIM_ATTACK;
					subaction = NANIM_ATTACK_2H_BASH;
					break;
				case ANIM_ATTACK_2H_BASH:
					action1 = NANIM_ATTACK;
					subaction = NANIM_ATTACK_2H_SLASH;
					break;
				case ANIM_CAST_DIR:
					action1 = NANIM_SPELL;
					subaction = NANIM_SPELL_NORMAL;
					break;
				case ANIM_CAST_AREA:
					action1 = NANIM_SPELL;
					subaction = NANIM_SPELL_SUMMON;
					break;
				case ANIM_ATTACK_BOW:
                case ANIM_HORSE_ATTACK_BOW:
					subaction = NANIM_ATTACK_BOW;
					break;
				case ANIM_ATTACK_XBOW:
                case ANIM_HORSE_ATTACK_XBOW:
					subaction = NANIM_ATTACK_CROSSBOW;
					break;
				case ANIM_GET_HIT:
					action1 = NANIM_GETHIT;
					break;
				case ANIM_BLOCK:
					action1 = NANIM_BLOCK;
					variation = 1;
					break;
				case ANIM_ATTACK_WRESTLE:
					action1 = NANIM_ATTACK;
					subaction = NANIM_ATTACK_WRESTLING;
					break;
				/*case ANIM_BOW:		//I'm commenting these 2 because they are not showing properly when Hovering/Mounted, so we skip them.
					action1 = NANIM_EMOTE;
					subaction = NANIM_EMOTE_BOW;
					break;
				case ANIM_SALUTE:
					action1 = NANIM_EMOTE;
					subaction = NANIM_EMOTE_SALUTE;
					break;*/
				case ANIM_EAT:
					action1 = NANIM_EAT;
					break;
				default:
					break;
			}
		}
	}//Other new animations that work on humans, elfs and gargoyles
	switch (action)
	{
		case ANIM_DIE_BACK:
			variation = 1;		//Variation makes characters die back
			action1 = NANIM_DEATH;
			break;
		case ANIM_DIE_FORWARD:
			action1 = NANIM_DEATH;
			break;
		default:
			break;
	}


	// New animation packet (PacketActionBasic): it supports some extra Gargoyle animations (and it can play Human/Elf animations), but lacks the animation "timing"/delay.
    //  EA always uses this packet for the Enhanced Client.

	// Old animation packet (PacketAction): doesn't really support Gargoyle animations; supported also by the Enhanced Client.
	//  On 2D/CC clients it can play Gargoyle animations, on Enhanced Client it can play some Gargoyle anims.
	//	On 2D/CC clients (even recent, Stygian Abyss ones) it supports the animation "timing"/delay, on Enhanced Client it has a fixed delay.

	PacketActionBasic* cmdnew = new PacketActionBasic(this, action1, subaction, variation);
	PacketAction* cmd = new PacketAction(this, action, 1, fBackward, iFrameDelay, iAnimLen);

	ClientIterator it;
	for (CClient* pClient = it.next(); pClient != nullptr; pClient = it.next())
	{
		if (!pClient->CanSee(this))
			continue;

		CNetState* state = pClient->GetNetState();
		if (state->isClientEnhanced() || state->isClientKR())
			cmdnew->send(pClient);
		else if (IsGargoyle() && state->isClientVersion(MINCLIVER_NEWMOBILEANIM))
			cmdnew->send(pClient);
		else
			cmd->send(pClient);
	}
	delete cmdnew;
	delete cmd;
	return true;
}

// If character status has been changed
// (Polymorph, war mode or hide), resend him
void CChar::UpdateMode( CClient * pExcludeClient, bool fFull )
{
	ADDTOCALLSTACK("CChar::UpdateMode");

	// no need to update the mode in the next tick
	if ( pExcludeClient == nullptr )
		m_fStatusUpdate &= ~SU_UPDATE_MODE;

	ClientIterator it;
	for ( CClient* pClient = it.next(); pClient != nullptr; pClient = it.next() )
	{
		if ( pExcludeClient == pClient )
			continue;
		if ( pClient->GetChar() == nullptr )
			continue;
		if ( GetTopPoint().GetDistSight(pClient->GetChar()->GetTopPoint()) > pClient->GetChar()->GetVisualRange() )
			continue;
		if ( !pClient->CanSee(this) )
		{
			// In the case of "INVIS" used by GM's we must use this.
			pClient->addObjectRemove(this);
			continue;
		}

        pClient->addChar(this, fFull);
	}
}

void CChar::UpdateSpeedMode()
{
	ADDTOCALLSTACK("CChar::UpdateSpeedMode");
	if ( g_Serv.IsLoading() || !m_pPlayer )
		return;

	if ( IsClientActive() )
		GetClientActive()->addSpeedMode( m_pPlayer->m_speedMode );
}

void CChar::UpdateVisualRange()
{
	ADDTOCALLSTACK("CChar::UpdateVisualRange");
	if ( g_Serv.IsLoading() || !m_pPlayer )
		return;

	DEBUG_WARN(("CChar::UpdateVisualRange called, m_iVisualRange is %d\n", m_iVisualRange));

	if ( IsClientActive() )
		GetClientActive()->addVisualRange( m_iVisualRange );
}

// Who now sees this char ?
// Did they just see him move ?
void CChar::UpdateMove( const CPointMap & ptOld, CClient * pExcludeClient, bool bFull )
{
	ADDTOCALLSTACK("CChar::UpdateMove");

	// no need to update the mode in the next tick
	if ( pExcludeClient == nullptr )
		m_fStatusUpdate &= ~SU_UPDATE_MODE;

	EXC_TRY("UpdateMove");
	EXC_SET_BLOCK("FOR LOOP");
	ClientIterator it;
	for ( CClient* pClient = it.next(); pClient != nullptr; pClient = it.next() )
	{
		if ( pClient == pExcludeClient )
			continue;	// no need to see self move

		if ( pClient == m_pClient )
		{
			EXC_SET_BLOCK("AddPlayerView");
			pClient->addPlayerView(ptOld, bFull);
			continue;
		}

		EXC_SET_BLOCK("GetChar");
		CChar * pChar = pClient->GetChar();
		if ( pChar == nullptr )
			continue;

		bool fCouldSee = (ptOld.GetDistSight(pChar->GetTopPoint()) <= pChar->GetVisualRange());
		EXC_SET_BLOCK("CanSee");
		if ( !pClient->CanSee(this) )
		{
			if ( fCouldSee )
			{
				EXC_SET_BLOCK("AddObjRem");
				pClient->addObjectRemove(this);		// this client can't see me anymore
			}
		}
		else if ( fCouldSee )
		{
			EXC_SET_BLOCK("AddcharMove");
			pClient->addCharMove(this);		// this client already saw me, just send the movement packet
		}
		else
		{
			EXC_SET_BLOCK("AddChar");
			pClient->addChar(this);			// first time this client has seen me, send complete packet
		}
	}
	EXC_CATCH;
}

// Change in direction.
void CChar::UpdateDir( DIR_TYPE dir )
{
	ADDTOCALLSTACK("CChar::UpdateDir (DIR_TYPE)");

	if ( dir != m_dirFace && dir > DIR_INVALID && dir < DIR_QTY )
	{
		m_dirFace = dir;	// face victim.
		UpdateMove(GetTopPoint());
	}
}

// Change in direction.
void CChar::UpdateDir( const CPointMap & pt )
{
	ADDTOCALLSTACK("CChar::UpdateDir (CPointMap)");

	UpdateDir(GetTopPoint().GetDir(pt));
}

// Change in direction.
void CChar::UpdateDir( const CObjBaseTemplate * pObj )
{
	ADDTOCALLSTACK("CChar::UpdateDir (CObjBaseTemplate)");
	if ( pObj == nullptr )
		return;

	pObj = pObj->GetTopLevelObj();
	if ( pObj == nullptr || pObj == this )		// in our own pack
		return;
	UpdateDir(pObj->GetTopPoint());
}

// If character status has been changed (Polymorph), resend him
// Or I changed looks.
// I moved or somebody moved me  ?
void CChar::Update(const CClient * pClientExclude )
{
	ADDTOCALLSTACK("CChar::Update");

	// no need to update the mode in the next tick
	if ( pClientExclude == nullptr)
		m_fStatusUpdate &= ~SU_UPDATE_MODE;

	ClientIterator it;
	for ( CClient* pClient = it.next(); pClient != nullptr; pClient = it.next() )
	{
		if ( pClient == pClientExclude )
			continue;
		if ( pClient->GetChar() == nullptr )
			continue;
		if ( GetTopPoint().GetDistSight(pClient->GetChar()->GetTopPoint()) > pClient->GetChar()->GetVisualRange() )
			continue;
		if ( !pClient->CanSee(this) )
		{
			// In the case of "INVIS" used by GM's we must use this.
			pClient->addObjectRemove(this);
			continue;
		}

		if ( pClient == m_pClient )
			pClient->addReSync();
		else
			pClient->addChar(this);
	}
}

// Make this char generate some sound according to the given action
void CChar::SoundChar( CRESND_TYPE type )
{
	ADDTOCALLSTACK("CChar::SoundChar");
	if ( !g_Cfg.m_fGenericSounds )
		return;

	if ((type < CRESND_RAND) || (type > CRESND_DIE))
	{
		DEBUG_WARN(("Invalid SoundChar type: %d.\n", (int)type));
		return;
	}

	SOUND_TYPE id = SOUND_NONE;

	// Am i hitting with a weapon?
	if ( type == CRESND_HIT )
	{
		CItem * pWeapon = m_uidWeapon.ItemFind();
		if ( pWeapon != nullptr )
		{
			//The Weapon_GetSoundHit() method below check if the ranged weapon equipped has the AMMOSOUNDHIT property.
			id = pWeapon->Weapon_GetSoundHit();
			if  ( !id )
			{
				// weapon type strike noise based on type of weapon and how hard hit.
				switch ( pWeapon->GetType() )
				{
					case IT_WEAPON_MACE_CROOK:
					case IT_WEAPON_MACE_PICK:
					case IT_WEAPON_MACE_SMITH:	// Can be used for smithing ?
					case IT_WEAPON_MACE_STAFF:
						// 0x233 = blunt01 (miss?)
						id = 0x233;
						break;
					case IT_WEAPON_MACE_SHARP:	// war axe can be used to cut/chop trees.
						// 0x232 = axe01 swing. (miss?)
						id = 0x232;
						break;
					case IT_WEAPON_SWORD:
					case IT_WEAPON_AXE:
						if ( pWeapon->Item_GetDef()->GetEquipLayer() == LAYER_HAND2 )
						{
							// 0x236 = hvyswrd1 = (heavy strike)
							// 0x237 = hvyswrd4 = (heavy strike)
							id = Calc_GetRandVal( 2 ) ? 0x236 : 0x237;
							break;
						}
						// if not two handed, don't break, just fall through and use the same sound ID as a fencing weapon
						FALLTHROUGH;
					case IT_WEAPON_FENCE:
						// 0x23b = sword1
						// 0x23c = sword7
						id = Calc_GetRandVal( 2 ) ? 0x23b : 0x23c;
						break;
					case IT_WEAPON_BOW:
					case IT_WEAPON_XBOW:
						// 0x234 = xbow (hit)
						id = 0x234;
						break;
					case IT_WEAPON_THROWING:
						// 0x5D2 = throwH
						id = 0x5D2;
						break;
                    case IT_WEAPON_WHIP:
                        id = 0x67E;		//whip01
                        break;
					default:
						break;
				}
			}

		}
	}

	if ( id == SOUND_NONE )	// i'm not hitting with a weapon
	{
		const CCharBase* pCharDef = Char_GetDef();
		if (type == CRESND_RAND)
			type = Calc_GetRandVal(2) ? CRESND_IDLE : CRESND_NOTICE;		// pick randomly CRESND_IDLE or CRESND_NOTICE

		// Do i have an override for this action sound?
		SOUND_TYPE idOverride = SOUND_NONE;
		switch (type)
		{
			case CRESND_IDLE:
				if (pCharDef->m_soundIdle)
					idOverride = pCharDef->m_soundIdle;
				break;
			case CRESND_NOTICE:
				if (pCharDef->m_soundNotice)
					idOverride = pCharDef->m_soundNotice;
				break;
			case CRESND_HIT:
				if (pCharDef->m_soundHit)
					idOverride = pCharDef->m_soundHit;
				break;
			case CRESND_GETHIT:
				if (pCharDef->m_soundGetHit)
					idOverride = pCharDef->m_soundGetHit;
				break;
			case CRESND_DIE:
				if (pCharDef->m_soundDie)
					idOverride = pCharDef->m_soundDie;
				break;
			default: break;
		}

		if (idOverride == (SOUND_TYPE)-1)
			return;		// if the override is = -1, the creature shouldn't play any sound for this action
		if (idOverride != SOUND_NONE)
			id = idOverride;
		else
		{
			// I have no override, check that i have a valid SOUND (m_soundBase) property.
			id = pCharDef->m_soundBase;
			switch ( id )
			{
				case SOUND_NONE:
					// some creatures have no base sounds, in this case i shouldn't even attempt to play them...
					//DEBUG_MSG(("CHARDEF %s has no base SOUND!\n", GetResourceName()));
					return;

				// Special (hardcoded) sounds
				case SOUND_SPECIAL_HUMAN:
				{
					static const SOUND_TYPE sm_Snd_Hit[] =
					{
						0x135,	//= hit01 = (slap)
						0x137,	//= hit03 = (hit sand)
						0x13b	//= hit07 = (hit slap)
					};
					static const SOUND_TYPE sm_Snd_Man_Die[] = { 0x15a, 0x15b, 0x15c, 0x15d };
					static const SOUND_TYPE sm_Snd_Man_Omf[] = { 0x154, 0x155, 0x156, 0x157, 0x158, 0x159 };
					static const SOUND_TYPE sm_Snd_Wom_Die[] = { 0x150, 0x151, 0x152, 0x153 };
					static const SOUND_TYPE sm_Snd_Wom_Omf[] = { 0x14b, 0x14c, 0x14d, 0x14e, 0x14f };

					if (type == CRESND_HIT)
					{
						id = sm_Snd_Hit[ Calc_GetRandVal( CountOf( sm_Snd_Hit )) ];		// same sound for every race and sex
					}
					else if ( pCharDef->IsFemale() )
					{
						switch ( type )
						{
							case CRESND_GETHIT:	id = sm_Snd_Wom_Omf[ Calc_GetRandVal( CountOf(sm_Snd_Wom_Omf)) ];	break;
							case CRESND_DIE:	id = sm_Snd_Wom_Die[ Calc_GetRandVal( CountOf(sm_Snd_Wom_Die)) ];	break;
							default:	break;
						}
					}
					else	// not CRESND_HIT and male character
					{
						switch ( type )
						{
							case CRESND_GETHIT:	id = sm_Snd_Man_Omf[ Calc_GetRandVal( CountOf(sm_Snd_Man_Omf)) ];	break;
							case CRESND_DIE:	id = sm_Snd_Man_Die[ Calc_GetRandVal( CountOf(sm_Snd_Man_Die)) ];	break;
							default:	break;
						}
					}
					// No idle/notice sounds for this.
				}
				break;

				// Every other sound
				default:
					if (id < 0x4D6)			// before the crane sound the sound IDs are ordered in a way...
						id += (SOUND_TYPE)type;
					else if (id < 0x5D5)	// starting with the crane and ending before absymal infernal there's another scheme
					{
						switch (type)
						{
							case CRESND_IDLE:	id += 2;	break;
							case CRESND_NOTICE:	id += 3;	break;
							case CRESND_HIT:	id += 1;	break;
							case CRESND_GETHIT:	id += 4;	break;
							case CRESND_DIE:				break;
							default: break;
						}
					}
					else					// staring with absymal infernal there's another scheme (and they have 4 sounds instead of 5)
					{
						switch (type)
						{
							case CRESND_IDLE:	id += 3;	break;
							case CRESND_NOTICE:	id += 3;	break;
							case CRESND_HIT:				break;
							case CRESND_GETHIT:	id += 2;	break;
							case CRESND_DIE:	id += 1;	break;
							default: break;
						}
					}
					break;
			}	// end of switch(id)
		}	// end of else
	}	// end of if ( id == SOUND_NONE )

	Sound(id);
}

// Pickup off the ground or remove my own equipment. etc..
// This item is now "up in the air"
// RETURN:
//  amount we can pick up.
//	-1 = we cannot pick this up.
int CChar::ItemPickup(CItem * pItem, word amount)
{
	ADDTOCALLSTACK("CChar::ItemPickup");

	if ( !pItem )
		return -1;

    CSObjCont* pItemParent = pItem->GetParent();
    const LAYER_TYPE iItemLayer = pItem->GetEquipLayer();
	if (pItemParent == this && iItemLayer == LAYER_HORSE )
		return -1;
	if ((pItemParent == this ) && (iItemLayer == LAYER_DRAGGING ))
		return pItem->GetAmount();
	if ( !CanTouch(pItem) || !CanMove(pItem, true) )
		return -1;

	CObjBaseTemplate * pObjTop = pItem->GetTopLevelObj();
    CChar* pCharTop = dynamic_cast<CChar*>(pObjTop);

	if( IsClientActive() )
	{
		CClient *client    = GetClientActive();
		CItem   *pItemCont = dynamic_cast <CItem*> (pItemParent);

		if ( pItemCont != nullptr )
		{
            const CPointMap& ptTop = GetTopPoint();
			// Don't allow taking items from the bank unless we opened it here
			if ( pItemCont->IsType( IT_EQ_BANK_BOX ) && ( pItemCont->m_itEqBankBox.m_pntOpen != ptTop) )
				return -1;

			// Check sub containers too
			if ( pCharTop != nullptr )
			{
                CItemContainer* pBank = pCharTop->GetBank();
				bool fItemContIsInsideBankBox = pBank->IsItemInside( pItemCont );
				if ( fItemContIsInsideBankBox && (pBank->m_itEqBankBox.m_pntOpen != ptTop))
					return -1;
			}

			// protect from snoop - disallow picking from not opened containers
			bool fIsInOpenedContainer = false;
			CClient::OpenedContainerMap_t::iterator itContainerFound = client->m_openedContainers.find( pItemCont->GetUID().GetPrivateUID() );
			if ( itContainerFound != client->m_openedContainers.end() )
			{
				const dword dwTopContainerUID = itContainerFound->second.first.first;
				const dword dwTopMostContainerUID = itContainerFound->second.first.second;
				const CPointMap& ptOpenedContainerPosition = itContainerFound->second.second;

				dword dwTopContainerUID_ToCheck = 0;

                const CObjBase* pCont = pItemCont->GetContainer();
                const CUID& UIDTop = pObjTop->GetUID();
				if (pCont)
					dwTopContainerUID_ToCheck = pCont->GetUID().GetPrivateUID();
				else
					dwTopContainerUID_ToCheck = UIDTop.GetPrivateUID();

				if ( ( dwTopMostContainerUID == UIDTop.GetPrivateUID() ) && ( dwTopContainerUID == dwTopContainerUID_ToCheck ) )
				{
					if ( pCharTop != nullptr )
					{
                        fIsInOpenedContainer = true;
						// probably a pickup check from pack if pCharTop != this?
					}
					else
					{
						CItem * pItemTop = dynamic_cast<CItem *>(pObjTop);
						if ( pItemTop && (pItemTop->IsType(IT_SHIP_HOLD) || pItemTop->IsType(IT_SHIP_HOLD_LOCK)) && (pItemTop->GetTopPoint().GetRegion(REGION_TYPE_MULTI) == GetTopPoint().GetRegion(REGION_TYPE_MULTI)) )
						{
                            fIsInOpenedContainer = true;
						}
						else if ( ptOpenedContainerPosition.GetDist( pObjTop->GetTopPoint() ) <= 3 )
						{
                            fIsInOpenedContainer = true;
						}
					}
				}
			}

			if (!fIsInOpenedContainer)
				return -1;
		}
	}

	if ( pCharTop != this &&
		pItem->IsAttr(ATTR_OWNED) &&
		pItem->m_uidLink != GetUID() &&
		!IsPriv(PRIV_ALLMOVE|PRIV_GM))
	{
		SysMessageDefault(DEFMSG_MSG_STEAL);
		return -1;
	}

	CItemCorpse * pCorpse = dynamic_cast<CItemCorpse *>(pObjTop);
	if (pCorpse)
	{
		if (pCorpse->m_uidLink == GetUID())
		{
			if (g_Cfg.m_iRevealFlags & REVEALF_LOOTINGSELF)
				Reveal();
		}
		else
		{
			CheckCorpseCrime(pCorpse, true, false);
			if (g_Cfg.m_iRevealFlags & REVEALF_LOOTINGOTHERS)
				Reveal();
		}
	}

	const word iAmountMax = pItem->GetAmount();
	if ( iAmountMax <= 0 )
		return -1;

	if ( !pItem->Item_GetDef()->IsStackableType() )
		amount = iAmountMax;	// it's not stackable, so we must pick up the entire amount
	else
		amount = maximum(1, minimum(amount, iAmountMax));

	//int iItemWeight = ( amount == iAmountMax ) ? pItem->GetWeight() : pItem->Item_GetDef()->GetWeight() * amount;
	int iItemWeight = pItem->GetWeight(amount);

	// Is it too heavy to even drag ?
	bool fDrop = false;
	if ( GetWeightLoadPercent(GetTotalWeight() + iItemWeight) > 300 )
	{
		SysMessageDefault(DEFMSG_MSG_HEAVY);
        if ((pCharTop == this) && (pItem->GetParent() == GetPack()))
            fDrop = true;	// we can always drop it out of own pack !
        else
            return -1;
	}

	ITRIG_TYPE trigger;
	if (pCharTop != nullptr )
	{
		bool fCanTake = false;
        if (IsPriv(PRIV_GM))// higher priv players can take items and undress us
        {
            fCanTake = (pCharTop == this) || (GetPrivLevel() > pCharTop->GetPrivLevel());
        }
        else if (pCharTop == this) // we can always take our own items
        {
            fCanTake = true;
        }
        else if ((pItem->GetContainer() != pCharTop) || (g_Cfg.m_fCanUndressPets == true)) // our owners can take items from us (with CanUndressPets=true, they can undress us too)
        {
            fCanTake = pCharTop->IsOwnedBy(this);
        }

		if (fCanTake == false)
		{
			SysMessageDefault(DEFMSG_MSG_STEAL);
			return -1;
		}
		trigger = pItem->IsItemEquipped() ? ITRIG_UNEQUIP : ITRIG_PICKUP_PACK;
	}
	else
	{
		trigger = pItem->IsTopLevel() ? ITRIG_PICKUP_GROUND : ITRIG_PICKUP_PACK;
	}

	if ( trigger == ITRIG_PICKUP_GROUND )
	{
		//	bug with taking static/movenever items -or- catching the spell effects
		if ( IsPriv(PRIV_ALLMOVE|PRIV_GM) ) ;
		else if ( pItem->IsAttr(ATTR_STATIC|ATTR_MOVE_NEVER) || pItem->IsType(IT_SPELL) )
			return -1;
	}

	if ( trigger != ITRIG_UNEQUIP )	// unequip is done later.
	{
		if (( IsTrigUsed(CItem::sm_szTrigName[trigger]) ) || ( IsTrigUsed(sm_szTrigName[(CTRIG_itemAfterClick - 1) + trigger]) )) //ITRIG_PICKUP_GROUND, ITRIG_PICKUP_PACK
		{
			CScriptTriggerArgs Args( amount );
			if ( pItem->OnTrigger( trigger, this, &Args ) == TRIGRET_RET_TRUE )
				return -1;
		}
		if (( trigger == ITRIG_PICKUP_PACK ) && (( IsTrigUsed(TRIGGER_PICKUP_SELF) ) || ( IsTrigUsed(TRIGGER_ITEMPICKUP_SELF) )))
		{
			CItem * pContItem = dynamic_cast <CItem*> ( pItem->GetContainer() );
			if ( pContItem )
			{
				CScriptTriggerArgs Args1(pItem);
				if ( pContItem->OnTrigger(ITRIG_PICKUP_SELF, this, &Args1) == TRIGRET_RET_TRUE )
					return -1;
			}
		}
	}


	if ( amount && pItem->Item_GetDef()->IsStackableType() && pItem->CanSendAmount() )
	{
		// Did we only pick up part of it ?
		// part or all of a pile. Only if pilable !
		if ( amount < iAmountMax )
		{
			// create left over item.
			CItem * pItemNew = pItem->UnStackSplit(amount, this);

			if (( IsTrigUsed(TRIGGER_PICKUP_STACK) ) || ( IsTrigUsed(TRIGGER_ITEMPICKUP_STACK) ))
			{
				CScriptTriggerArgs Args2(pItemNew);
				if ( pItem->OnTrigger(ITRIG_PICKUP_STACK, this, &Args2) == TRIGRET_RET_TRUE )
					return -1;
			}

		}
	}

	// Do stack dropping if items are stacked
	if (( trigger == ITRIG_PICKUP_GROUND ) && IsSetEF( EF_ItemStackDrop ))
	{
		char iItemHeight = pItem->GetHeight();
		iItemHeight = maximum(iItemHeight, 1);
		char iStackMaxZ = GetTopZ() + 16;
		CItem * pStack = nullptr;
		CPointMap ptNewPlace = pItem->GetTopPoint();
		CWorldSearch AreaItems(ptNewPlace);
		for (;;)
		{
			pStack = AreaItems.GetItem();
			if ( pStack == nullptr )
				break;
			if (( pStack->GetTopZ() <= pItem->GetTopZ()) || ( pStack->GetTopZ() > iStackMaxZ ))
				continue;
			if ( pStack->IsAttr(ATTR_MOVE_NEVER|ATTR_STATIC|ATTR_LOCKEDDOWN))
				continue;

			ptNewPlace = pStack->GetTopPoint();
			ptNewPlace.m_z -= iItemHeight;
			pStack->MoveToUpdate(ptNewPlace);
		}
	}

	if ( fDrop )
	{
		ItemDrop(pItem, GetTopPoint());
		return -1;
	}

	// do the dragging anim for everyone else to see.
	UpdateDrag(pItem);

	// Remove the item from other clients view if the item is
	// being taken from the ground by a hidden character to
	// prevent lingering item.
	if ( ( trigger == ITRIG_PICKUP_GROUND ) && (IsStatFlag( STATF_INSUBSTANTIAL | STATF_INVISIBLE | STATF_HIDDEN )) )
        pItem->RemoveFromView( m_pClient );

	// Pick it up.
	pItem->SetDecayTime(-1);	// Kill any decay timer.
	CCSpawn * pSpawn = GetSpawn();
	if (pSpawn)
		pSpawn->DelObj(pItem->GetUID());
	LayerAdd( pItem, LAYER_DRAGGING );

	return amount;
}

// We can't put this where we want to
// So put in my pack if i can. else drop.
// don't check where this came from !
bool CChar::ItemBounce( CItem * pItem, bool fDisplayMsg )
{
	ADDTOCALLSTACK("CChar::ItemBounce");
	if ( pItem == nullptr )
		return false;

	CItemContainer * pPack = GetPackSafe();
	if ( pItem->GetParent() == pPack )
		return true;

	lpctstr pszWhere = nullptr;
	bool fCanAddToPack = false;
    bool fDropOnGround = false;

	if (pPack && CanCarry(pItem) && pPack->CanContainerHold(pItem, this))		// this can happen at load time
	{
		fCanAddToPack = true;
		if (IsTrigUsed(TRIGGER_DROPON_ITEM))
		{
			CScriptTriggerArgs Args(pPack);
			pItem->OnTrigger(ITRIG_DROPON_ITEM, this, &Args);

			if (pItem->IsDeleted())	// the trigger had deleted the item
				return false;
		}

		if (IsTrigUsed(TRIGGER_DROPON_SELF) || IsTrigUsed(TRIGGER_ITEMDROPON_SELF))
		{
            const CItem* pPrevCont = dynamic_cast<CItem*>(pItem->GetContainer());
			CScriptTriggerArgs Args(pItem);
            const TRIGRET_TYPE ret = pPack->OnTrigger(ITRIG_DROPON_SELF, this, &Args);
            if ( pItem->IsDeleted() )	// the trigger had deleted the item
                return false;
			if (ret == TRIGRET_RET_TRUE)
            {
				fCanAddToPack = false;
                const CItem* pCont = dynamic_cast<const CItem*>(pItem->GetContainer());
				if (pPrevCont == pCont) //In the same cont, but unable to go there
					fDropOnGround = true;
				else //we changed the cont in the script
				{
					tchar* pszMsg = Str_GetTemp();
					snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_MSG_BOUNCE_CONT), pCont->GetName());
					pszWhere = pszMsg;
				}
            }
		}
	}
    else
    {
        fDropOnGround = true;
    }

	if (fCanAddToPack)
	{
		pszWhere = g_Cfg.GetDefaultMsg( DEFMSG_MSG_BOUNCE_PACK );
		pItem->RemoveFromView();
		pPack->ContentAdd(pItem);		// add it to pack
	}
	else if (fDropOnGround)
	{
		if ( !GetTopPoint().IsValidPoint() )
		{
			// NPC is being created and has no valid point yet.
			if ( pszWhere )
				DEBUG_ERR(("No pack to place loot item '%s' for NPC '%s'\n", pItem->GetResourceName(), GetResourceName()));
			else
				DEBUG_ERR(("Loot item '%s' too heavy for NPC '%s'\n", pItem->GetResourceName(), GetResourceName()));

			pItem->Delete();
			return false;
		}

        const CObjBase* pItemContPrev = pItem->GetContainer();
        int64 iDecayTime = pItem->GetDecayTime();
        CPointMap ptDrop(GetTopPoint());
        TRIGRET_TYPE ttResult = TRIGRET_RET_DEFAULT;
        if (IsTrigUsed(TRIGGER_DROPON_GROUND) || IsTrigUsed(TRIGGER_ITEMDROPON_GROUND))
        {
            CScriptTriggerArgs args;
            args.m_iN1 = iDecayTime / MSECS_PER_TENTH;  // ARGN1 = Decay time for the dropped item (in tenths of second)
            args.m_iN2 = 1;
            args.m_s1 = ptDrop.WriteUsed();
            ttResult = pItem->OnTrigger(ITRIG_DROPON_GROUND, this, &args);

            if (IsDeleted())
                return false;

            iDecayTime = args.m_iN1 * MSECS_PER_TENTH;

			// Warning: here we ignore the read-onlyness of CSString's buffer only because we know that CPointMap constructor won't write past the end, but only replace some characters with '\0'. It's not worth it to build another string just for that.
			tchar* ptcArgs = const_cast<tchar*>(args.m_s1.GetBuffer());
			const CPointMap ptDropNew(ptcArgs);
            if (!ptDropNew.IsValidPoint())
                g_Log.EventError("Trying to override item drop P with an invalid P. Using the original one.\n");
            else
                ptDrop = ptDropNew;
        }

        UNREFERENCED_PARAMETER(ttResult);
        /*
        // RETURN 1 has a different meaning when used in a trigger call not generated by a bounce check, so it's better to avoid this additional behaviour
        //  (which, anyways, can be easily achieved via scripts)
        if (ttResult == TRIGRET_RET_TRUE)
        {
            if (pItem->IsTopLevel())
                g_Log.EventError("Can't prevent BOUNCEing to ground an item which hasn't a previous container.\n");
            else
                return false;
        }
        */

        // If i have changed the container via scripts, we shouldn't change item's position again here
        if (pItemContPrev == pItem->GetContainer())
        {
            pszWhere = g_Cfg.GetDefaultMsg(DEFMSG_MSG_FEET);
            fDisplayMsg = true;
            pItem->RemoveFromView();
            pItem->MoveToDecay(GetTopPoint(), iDecayTime);	// drop it on ground
        }
	}

	if (!IsStatFlag(STATF_DEAD | STATF_CONJURED))
	{
		// Ensure i am not summon, or inside CreateLoot trigger
		Sound(pItem->GetDropSound(pPack));
	}
	if (fDisplayMsg)
		SysMessagef( g_Cfg.GetDefaultMsg( DEFMSG_MSG_ITEMPLACE ), pItem->GetName(), pszWhere );
	return true;
}

// A char actively drops an item on the ground.
bool CChar::ItemDrop( CItem * pItem, const CPointMap & pt )
{
	ADDTOCALLSTACK("CChar::ItemDrop");
	if ( pItem == nullptr )
		return false;

	if ( IsSetEF( EF_ItemStacking ) )
	{
		char iItemHeight = pItem->GetHeight();
		CServerMapBlockState block( CAN_C_WALK, pt.m_z, pt.m_z, pt.m_z, maximum(iItemHeight,1) );
		//CWorldMap::GetHeightPoint( pt, block, true );
		//DEBUG_ERR(("Drop: %d / Min: %d / Max: %d\n", pItem->GetFixZ(pt), block.m_Bottom.m_z, block.m_Top.m_z));

		CPointMap ptStack = pt;
		const char iStackMaxZ = block.m_Top.m_z;	//pt.m_z + 16;
		const CItem * pStack = nullptr;
		CWorldSearch AreaItems(ptStack);
		pStack = AreaItems.GetItem();
		if (pStack != nullptr) //If there nothing  on the ground, drop the item normally and flip it if it's possible
		{
			for (uint i = 0;; ++i)
			{
				if (i != 0) //on first iteration, pStack already contain the item on the ground. If you getitem again, you'll obtain nullptr
				{
					pStack = AreaItems.GetItem();
				}
				if (pStack == nullptr)
				{
					break;
				}
				const char iStackZ = pStack->GetTopZ();
				if (iStackZ < pt.m_z || iStackZ > iStackMaxZ )
					continue;

				const short iStackHeight = pStack->GetHeight();
				ptStack.m_z += (char)maximum(iStackHeight, 1);
				//DEBUG_ERR(("(%d > %d) || (%d > %d)\n", ptStack.m_z, iStackMaxZ, ptStack.m_z + maximum(iItemHeight, 1), iStackMaxZ + 3));
				if ( (ptStack.m_z > iStackMaxZ) || (ptStack.m_z + maximum(iItemHeight, 1) > iStackMaxZ + 3) )
				{
					ItemBounce( pItem );		// put the item on backpack (or drop it on ground if it's too heavy)
					return false;
				}
			}
			return pItem->MoveToCheck( ptStack, this );	// don't flip the item if it got stacked
		}
	}

	// Does this item have a flipped version?
	CItemBase * pItemDef = pItem->Item_GetDef();
	if (( g_Cfg.m_fFlipDroppedItems && pItem->Can(CAN_I_FLIP)) && pItem->IsMovableType() && !pItemDef->IsStackableType())
		pItem->SetDispID( pItemDef->GetNextFlipID( pItem->GetDispID()));

	return pItem->MoveToCheck( pt, this );
}

// Equip visible stuff. else throw into our pack.
// Pay no attention to where this came from.
// Bounce anything in the slot we want to go to. (if possible)
// Adding 'equip benefics' to the char
// NOTE: This can be used from scripts as well to equip memories etc.
// ASSUME this is ok for me to use. (movable etc)
bool CChar::ItemEquip( CItem * pItem, CChar * pCharMsg, bool fFromDClick )
{
	ADDTOCALLSTACK("CChar::ItemEquip");

	if ( !pItem )
		return false;

	// In theory someone else could be dressing me ?
	if ( !pCharMsg )
		pCharMsg = this;

	if ( pItem->GetParent() == this )
	{
		if ( pItem->GetEquipLayer() != LAYER_DRAGGING ) // already equipped.
			return true;
	}

	LAYER_TYPE layer = CanEquipLayer(pItem, LAYER_QTY, pCharMsg, false);
	if ( layer == LAYER_NONE )	// if this isn't an equippable item or if i can't equip it
	{
		// Only bounce to backpack if NPC, because players will call CClient::Event_Item_Drop_Fail() to drop the item back on its last location.
		// But, for clients, bounce it if we are trying to equip the item without an explicit client request (character creation, equipping from scripts or source..)
		if ( m_pNPC || (!m_pNPC && !fFromDClick) )
			ItemBounce(pItem);
		return false;
	}

	if (IsTrigUsed(TRIGGER_EQUIPTEST) || IsTrigUsed(TRIGGER_ITEMEQUIPTEST))
	{
		if (pItem->OnTrigger(ITRIG_EQUIPTEST, this) == TRIGRET_RET_TRUE)
		{
			// since this trigger is called also when creating an item via ITEM=, if the created item has a RETURN 1 in @EquipTest
			// (or if the NPC has a RETURN 1 in @ItemEquipTest), the item will be created but not placed in the world.
			// so, if this is an NPC, even if there's a RETURN 1 i need to bounce the item inside his pack

			//if (m_pNPC && (pItem->GetTopLevelObj() == this) )		// use this if we want to bounce the item only if i have picked it up previously (so it isn't valid if picking up from the ground)
			if (m_pNPC && !pItem->IsDeleted())
				ItemBounce(pItem);
			return false;
		}

		if (pItem->IsDeleted())
			return false;
	}

	// strong enough to equip this . etc ?
	// Move stuff already equipped.
	if (pItem->GetAmount() > 1)
		pItem->UnStackSplit(1, this);

	pItem->RemoveSelf();		// Remove it from the container so that nothing will be stacked with it if unequipped
	pItem->SetDecayTime(-1);	// Kill any DECAY timer.
	LayerAdd(pItem, layer);
	if ( !pItem->IsItemEquipped() )	// Equip failed ? (cursed?) Did it just go into pack ?
		return false;

	if (( IsTrigUsed(TRIGGER_EQUIP) ) || ( IsTrigUsed(TRIGGER_ITEMEQUIP) ))
	{
		if ( pItem->OnTrigger(ITRIG_EQUIP, this) == TRIGRET_RET_TRUE )
			return false;
	}

	if ( !pItem->IsItemEquipped() )	// Equip failed ? (cursed?) Did it just go into pack ?
		return false;

    if (CItemBase::IsVisibleLayer(layer))	// visible layer ?
    {
        SOUND_TYPE iSound = 0x57;
        CVarDefCont * pVar = GetDefKey("EQUIPSOUND", true);
        if ( pVar )
        {
            int64 iVal = pVar->GetValNum();
            if ( iVal )
                iSound = (SOUND_TYPE)iVal;
        }
        Sound(iSound);
    }

	if ( fFromDClick )
		pItem->ResendOnEquip();


    const CBaseBaseDef* pItemBase = pItem->Base_GetDef();

    // Start of CCPropsItemEquippable props
	CCPropsChar* pCCPChar = GetComponentProps<CCPropsChar>();
	CCPropsItemEquippable* pItemCCPItemEquippable = pItem->GetComponentProps<CCPropsItemEquippable>();
	const CCPropsItemEquippable* pItemBaseCCPItemEquippable = pItemBase->GetComponentProps<CCPropsItemEquippable>();

    if (pItemCCPItemEquippable || pItemBaseCCPItemEquippable)
    {
        // Leave the stat bonuses signed, since they can be used also as a malus (negative sign)
        Stat_AddMod(STAT_STR,       + (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSSTR, pItemBaseCCPItemEquippable));
        Stat_AddMod(STAT_DEX,       + (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSDEX, pItemBaseCCPItemEquippable));
        Stat_AddMod(STAT_INT,       + (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSINT, pItemBaseCCPItemEquippable));
        Stat_AddMaxMod(STAT_STR,    + (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSHITSMAX, pItemBaseCCPItemEquippable));
        Stat_AddMaxMod(STAT_DEX,    + (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSSTAMMAX, pItemBaseCCPItemEquippable));
        Stat_AddMaxMod(STAT_INT,    + (int)pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_BONUSMANAMAX, pItemBaseCCPItemEquippable));

        ModPropNum(pCCPChar, PROPCH_RESPHYSICAL,  + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESPHYSICAL, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESFIRE,      + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESFIRE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESCOLD,      + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESCOLD, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESPOISON,    + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESPOISON, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_RESENERGY,    + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_RESENERGY, pItemBaseCCPItemEquippable));

        ModPropNum(pCCPChar, PROPCH_INCREASEDAM,          + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASEDAM, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASEDEFCHANCE,    + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASEDEFCHANCE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_FASTERCASTING,        + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_FASTERCASTING, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASEHITCHANCE,    + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASEHITCHANCE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASESPELLDAM,     + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASESPELLDAM, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_INCREASESWINGSPEED,   + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_INCREASESWINGSPEED, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_ENHANCEPOTIONS,       + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_ENHANCEPOTIONS, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_LOWERMANACOST,        + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_LOWERMANACOST, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_LUCK,                 + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_LUCK, pItemBaseCCPItemEquippable));

        ModPropNum(pCCPChar, PROPCH_DAMPHYSICAL,   + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMPHYSICAL, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMFIRE,       + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMFIRE, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMCOLD,       + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMCOLD, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMPOISON,     + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMPOISON, pItemBaseCCPItemEquippable));
        ModPropNum(pCCPChar, PROPCH_DAMENERGY,     + pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_DAMENERGY, pItemBaseCCPItemEquippable));

        if ( pItem->GetPropNum(pItemCCPItemEquippable, PROPIEQUIP_NIGHTSIGHT, pItemBaseCCPItemEquippable) )
        {
            StatFlag_Mod( STATF_NIGHTSIGHT, 1 );
            if ( IsClientActive() )
                m_pClient->addLight();
        }

        if (pItem->IsTypeWeapon())
        {
            CItem * pCursedMemory = LayerFind(LAYER_SPELL_Curse_Weapon);	// Remove the cursed state from SPELL_Curse_Weapon.
            if (pCursedMemory)
                pItem->ModPropNum(pItemCCPItemEquippable, PROPIEQUIP_HITLEECHLIFE, + pCursedMemory->m_itSpell.m_spelllevel, pItemBaseCCPItemEquippable);
        }
    }
    // End of CCPropsItemEquippable props

    Spell_Effect_Add(pItem);	// if it has a magic effect.

	return true;
}

// OnEat()
// Generating eating animation
// also calling @Eat and setting food's level (along with other possible stats 'local.hits',etc?)
void CChar::EatAnim( lpctstr pszName, ushort uiQty )
{
	ADDTOCALLSTACK("CChar::EatAnim");
	static const SOUND_TYPE sm_EatSounds[] = { 0x03a, 0x03b, 0x03c };
	Sound(sm_EatSounds[Calc_GetRandVal(CountOf(sm_EatSounds))]);

	if ( !IsStatFlag(STATF_ONHORSE) )
		UpdateAnimate(ANIM_EAT);

	tchar * pszMsg = Str_GetTemp();
	snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_MSG_EATSOME), pszName);
	Emote(pszMsg);

	ushort uiHits = 0;
	ushort uiMana = 0;
	ushort uiStam = (ushort)( Calc_GetRandVal2(3, 6) + (uiQty / 5) );
	ushort uiFood = uiQty;
	ushort uiStatsLimit = 0;
	if ( IsTrigUsed(TRIGGER_EAT) )
	{
		CScriptTriggerArgs Args;
		Args.m_VarsLocal.SetNumNew("Hits", uiHits);
		Args.m_VarsLocal.SetNumNew("Mana", uiMana);
		Args.m_VarsLocal.SetNumNew("Stam", uiStam);
		Args.m_VarsLocal.SetNumNew("Food", uiFood);
		Args.m_iN1 = uiStatsLimit;
		if ( OnTrigger(CTRIG_Eat, this, &Args) == TRIGRET_RET_TRUE )
			return;

		uiHits = (ushort)(Args.m_VarsLocal.GetKeyNum("Hits")) + Stat_GetVal(STAT_STR);
		uiMana = (ushort)(Args.m_VarsLocal.GetKeyNum("Mana")) + Stat_GetVal(STAT_INT);
		uiStam = (ushort)(Args.m_VarsLocal.GetKeyNum("Stam")) + Stat_GetVal(STAT_DEX);
		uiFood = (ushort)(Args.m_VarsLocal.GetKeyNum("Food")) + Stat_GetVal(STAT_FOOD);
		uiStatsLimit = (ushort)(Args.m_iN1);
	}

	if ( uiHits )
		UpdateStatVal(STAT_STR, uiHits, uiStatsLimit);
	if ( uiMana )
		UpdateStatVal(STAT_INT, uiMana, uiStatsLimit);
	if ( uiStam )
		UpdateStatVal(STAT_DEX, uiStam, uiStatsLimit);
	if ( uiFood )
		UpdateStatVal(STAT_FOOD, uiFood, uiStatsLimit);
}

// Some outside influence may be revealing us.
// -1 = reveal everything, also invisible GMs
bool CChar::Reveal( uint64 iFlags )
{
	ADDTOCALLSTACK("CChar::Reveal");

	if ( !iFlags)
        iFlags = STATF_INVISIBLE|STATF_HIDDEN|STATF_SLEEPING;
	if ( !IsStatFlag(iFlags) )
		return false;

    CClient* pClient = IsClientActive() ? GetClientActive() : nullptr;
	if (pClient && pClient->m_pHouseDesign)
	{
		// No reveal whilst in house design (unless they somehow got out)
		if (pClient->m_pHouseDesign->GetDesignArea().IsInside2d(GetTopPoint()) )
			return false;

        pClient->m_pHouseDesign->EndCustomize(true);
	}

	if ( (iFlags & STATF_SLEEPING) && IsStatFlag(STATF_SLEEPING) )
		Wake();

	if ( (iFlags & STATF_INVISIBLE) && IsStatFlag(STATF_INVISIBLE) )
	{
		CItem * pSpell = LayerFind(LAYER_SPELL_Invis);
		if ( pSpell && pSpell->IsType(IT_SPELL) && (pSpell->m_itSpell.m_spell == SPELL_Invis) )
		{
			pSpell->SetType(IT_NORMAL);		// setting it to IT_NORMAL avoid a second call to this function
			pSpell->Delete();
		}
		pSpell = LayerFind(LAYER_FLAG_Potion);
		if ( pSpell && pSpell->IsType(IT_SPELL) && (pSpell->m_itSpell.m_spell == SPELL_Invis) )
		{
			pSpell->SetType(IT_NORMAL);		// setting it to IT_NORMAL avoid a second call to this function
			pSpell->Delete();
		}
	}

	StatFlag_Clear(iFlags);
	if ( pClient )
	{
		if ( !IsStatFlag(STATF_HIDDEN|STATF_INSUBSTANTIAL) )
			pClient->removeBuff(BI_HIDDEN);
		if ( !IsStatFlag(STATF_INVISIBLE) )
			pClient->removeBuff(BI_INVISIBILITY);
	}

	if ( IsStatFlag(STATF_INVISIBLE|STATF_HIDDEN|STATF_INSUBSTANTIAL|STATF_SLEEPING) )
		return false;

	m_StepStealth = 0;
	UpdateMode(nullptr, true);
	SysMessageDefault(DEFMSG_HIDING_REVEALED);
	return true;
}

void CChar::Speak_RevealCheck(TALKMODE_TYPE mode)
{
    ADDTOCALLSTACK("CChar::Spell_RevealCheck");
    if (mode != TALKMODE_SPELL)
    {
        if (GetKeyNum("OVERRIDE.NOREVEALSPEAK", true) != 1)
        {
            if (g_Cfg.m_iRevealFlags & REVEALF_SPEAK)	// spell's reveal is handled in Spell_CastStart
                Reveal();
        }
    }
}

// Player: Speak to all clients in the area.
// Ignore the font argument here !
// ASCII packet
void CChar::Speak(lpctstr pszText, HUE_TYPE wHue, TALKMODE_TYPE mode, FONT_TYPE font)
{
	ADDTOCALLSTACK("CChar::Speak");

	if (IsStatFlag(STATF_STONE))
		return;
	if ((mode == TALKMODE_YELL) && (GetPrivLevel() >= PLEVEL_Counsel))
		mode = TALKMODE_BROADCAST;					// GM Broadcast (done if a GM yells something)

    Speak_RevealCheck(mode);

	CObjBase::Speak(pszText, wHue, mode, font);
}

// Player: Speak to all clients in the area.
// Ignore the font argument here !
// Unicode packet
void CChar::SpeakUTF8( lpctstr pszText, HUE_TYPE wHue, TALKMODE_TYPE mode, FONT_TYPE font, CLanguageID lang )
{
	ADDTOCALLSTACK("CChar::SpeakUTF8");

	if ( IsStatFlag(STATF_STONE) )
		return;
	if ((mode == TALKMODE_YELL) && (GetPrivLevel() >= PLEVEL_Counsel))
		mode = TALKMODE_BROADCAST;					// GM Broadcast (done if a GM yells something)

    Speak_RevealCheck(mode);

	CObjBase::SpeakUTF8(pszText, wHue, mode, font, lang);
}

// Player: Speak to all clients in the area.
// Ignore the font argument here !
// Unicode packet
// Difference with SpeakUTF8: this method accepts as text input an nword, which is a unicode character if sphere is compiled with UNICODE macro)
void CChar::SpeakUTF8Ex( const nword * pszText, HUE_TYPE wHue, TALKMODE_TYPE mode, FONT_TYPE font, CLanguageID lang )
{
	ADDTOCALLSTACK("CChar::SpeakUTF8Ex");

	if ( IsStatFlag(STATF_STONE) )
		return;
	if ((mode == TALKMODE_YELL) && (GetPrivLevel() >= PLEVEL_Counsel))
		mode = TALKMODE_BROADCAST;

    Speak_RevealCheck(mode);

	CObjBase::SpeakUTF8Ex(pszText, wHue, mode, font, lang);
}

// Convert me into a figurine
CItem * CChar::Make_Figurine( const CUID &uidOwner, ITEMID_TYPE id )
{
	ADDTOCALLSTACK("CChar::Make_Figurine");
	if ( IsDisconnected() || m_pPlayer )
		return nullptr;

	CCharBase* pCharDef = Char_GetDef();

	// turn creature into a figurine.
	CItem * pItem = CItem::CreateScript( (id == ITEMID_NOTHING) ? pCharDef->m_trackID : id, this );
	if ( !pItem )
		return nullptr;

	pItem->SetType(IT_FIGURINE);
	pItem->SetName(GetName());
	pItem->SetHue(GetHue());
	pItem->m_itFigurine.m_ID = GetID();	// Base type of creature.
	pItem->m_itFigurine.m_UID = GetUID();
	pItem->m_uidLink = uidOwner;

	if ( IsStatFlag(STATF_INSUBSTANTIAL) )
		pItem->SetAttr(ATTR_INVIS);

	SoundChar(CRESND_IDLE);			// Horse winny
	StatFlag_Set(STATF_RIDDEN);
	Skill_Start(NPCACT_RIDDEN);
	SetDisconnected();
	m_atRidden.m_uidFigurine = pItem->GetUID();

	return pItem;
}

// Call Make_Figurine() and place me
// This will just kill conjured creatures.
CItem * CChar::NPC_Shrink()
{
	ADDTOCALLSTACK("CChar::NPC_Shrink");
	if ( IsStatFlag(STATF_CONJURED) )
	{
		Stat_SetVal(STAT_STR, 0);
		return nullptr;
	}

	NPC_PetClearOwners();	// Clear follower slots on pet owner

	CItem * pItem = Make_Figurine(CUID(), ITEMID_NOTHING);
	if ( !pItem )
		return nullptr;

	pItem->SetAttr(ATTR_MAGIC);
	pItem->MoveToCheck(GetTopPoint());
	return pItem;
}


// I am a horse.
// Get my mount object. (attached to my rider)
CItem* CChar::Horse_GetMountItem() const
{
    ADDTOCALLSTACK("CChar::Horse_GetMountItem");

    ASSERT(STATF_RIDDEN);   // This function should be called only on mounts.
	if (!IsDisconnected() || (Skill_GetActive() != NPCACT_RIDDEN))
		return nullptr;

    CItem* pMountItem = m_atRidden.m_uidFigurine.ItemFind();   // ACTARG1 = Mount item UID
    if (pMountItem)
    {
		if (pMountItem->IsType(IT_FIGURINE))	// It's a shrinked npc
		{
			if (pMountItem->m_itFigurine.m_UID == GetUID())
				return pMountItem;
			return nullptr;
		}

		if (pMountItem->IsType(IT_EQ_HORSE))	// It's a mount item
		{
			const CChar* pRider = dynamic_cast<const CChar*>(pMountItem->GetTopLevelObj());
			if (pRider)
			{
				if (!IsStatFlag(STATF_PET) && !pRider->IsPriv(PRIV_GM) && (pRider->GetPrivLevel() <= PLEVEL_Player))
					return nullptr;

				const CItem* pOwnerMountItem = pRider->LayerFind(LAYER_HORSE);
				if (pOwnerMountItem && (pOwnerMountItem == pMountItem))
					return pMountItem;
			}
		}
    }
    return nullptr;
}

CItem* CChar::Horse_GetValidMountItem()
{
    ADDTOCALLSTACK("CChar::Horse_GetValidMountItem");

    ASSERT(STATF_RIDDEN);
	if (!IsDisconnected() || (Skill_GetActive() != NPCACT_RIDDEN))
		return nullptr;

    CItem* pMountItem = Horse_GetMountItem();
    if (pMountItem)
        return pMountItem;

    // Try to auto-fix the mount item/figurine, which at this point should be invalid.
    int iFailureCode = 0;
	int iFixCode = 0;

	const CUID& uidThis = GetUID();
	pMountItem = m_atRidden.m_uidFigurine.ItemFind();

	if (pMountItem && pMountItem->IsType(IT_FIGURINE))
	{
		if (pMountItem->m_itFigurine.m_UID != uidThis)
		{
			pMountItem->m_itFigurine.m_UID = uidThis;
			// Assume pMountItem->m_itFigurine.m_ID is correct?

			g_Log.EventWarn("Mount (UID=0%x, id=0%x '%s'): Fixed mislinked figurine with UID=ACTARG1=0%x, id=0%x '%s'\n",
				(dword)GetUID(), GetBaseID(), GetName(),
				(dword)(pMountItem->GetUID()), pMountItem->GetBaseID(), pMountItem->GetName());
		}

		return pMountItem;
	}

	if (pMountItem && !pMountItem->IsType(IT_EQ_HORSE))
		return nullptr;

	const CChar* pOwner = NPC_PetGetOwner();
	if (!pMountItem && pOwner)
	{
		// Invalid ACTARG1 (which should hold the UID of the mount item).
		// Let's try to find and check owner's mount item equipped in LAYER_HORSE.
		pMountItem = pOwner->LayerFind(LAYER_HORSE);
		if (pMountItem)
		{
			if (pMountItem->m_itFigurine.m_UID == uidThis)
			{
				// Horse linked to an invalid figurine (broken ACTARG1?)
				iFixCode = 1;
				m_atRidden.m_uidFigurine = pMountItem->GetUID();
			}
			else
			{
				const CChar* pLinkedChar = pMountItem->m_uidLink.CharFind();
				if (pLinkedChar && pLinkedChar->IsStatFlag(STATF_RIDDEN))
				{
					// Owner has a mount item linked to another, valid mount, so don't touch that
					iFailureCode = 2;
				}
				else
				{
					iFixCode = 2;
					pMountItem->m_itFigurine.m_UID = uidThis;
					pMountItem->m_uidLink = pOwner->GetUID();
					m_atRidden.m_uidFigurine = pMountItem->GetUID();
				}
			}
		}
		else
			iFailureCode = 1;
	}
	else if (pMountItem)
    {
        const CChar* pRider = dynamic_cast<CChar*>(pMountItem->GetTopLevelObj());
        if (!pRider && pOwner)
            pRider = pOwner;

        if (pRider && pRider->IsStatFlag(STATF_ONHORSE))
        {
            CItem* pRiderMountItem = pRider->LayerFind(LAYER_HORSE);
            if (pRiderMountItem)
            {
                if (pRider && (pRider != pOwner))
                {
                    // Horse linked (by ACTARG1) to a wrong figurine (which is equipped by another character).
                    if (pRiderMountItem->m_itFigurine.m_UID == uidThis)
                    {
                        // Fixable.
                        iFixCode = 3;
                        m_atRidden.m_uidFigurine = pRiderMountItem->GetUID();
                    }
                    else
                    {
                        // Completely broken.
                        iFailureCode = 6;
                    }
                }
                else if (pRiderMountItem != pMountItem)
                {
                    if (pMountItem->IsType(IT_FIGURINE) || pMountItem->IsType(IT_EQ_HORSE))
                    {
                        // Be sure that the ACTARG1 isn't just plain corrupt and do not remove random items, but only invalid mount items
                        pMountItem->Delete();
                    }
                    pMountItem = pRiderMountItem;

                    if (pMountItem->m_itFigurine.m_UID == uidThis)
                    {
                        // Horse linked (by ACTARG1) to a wrong figurine.
                        iFixCode = 4;
                        m_atRidden.m_uidFigurine = pMountItem->GetUID();
                    }
                    else if (pMountItem->IsType(IT_FIGURINE) || pMountItem->IsType(IT_EQ_HORSE))
                    {
						const CChar* pLinkedChar = pMountItem->m_uidLink.CharFind();
						if (pLinkedChar && pLinkedChar->IsStatFlag(STATF_RIDDEN))
						{
							// Owner has a mount item linked to another, valid mount, so don't touch that
							iFailureCode = 7;
						}
						else
						{
							iFixCode = 5;
							pMountItem->m_itFigurine.m_UID = uidThis;
							pMountItem->m_uidLink = pRider->GetUID();
							m_atRidden.m_uidFigurine = pMountItem->GetUID();
						}
                    }
                    else
                        iFailureCode = 8;
                }
                else
                    iFailureCode = 5;
            }
            else
            {
                // Horse owner==rider doesn't have a mount item...
                iFailureCode = 4;
            }
        }
        else
            iFailureCode = 3;

		if (!iFailureCode)
		{
			ASSERT(iFixCode);
			pMountItem->m_itFigurine.m_ID = GetID();

			g_Log.EventWarn("Mount (UID=0%x, id=0%x '%s'): Fixed mount item (mount item UID=ACTARG1=0%x) with UID=0%x, id=0%x '%s'\n",
				(dword)GetUID(), GetBaseID(), GetName(), (dword)m_atRidden.m_uidFigurine,
				(dword)(pMountItem->GetUID()), pMountItem->GetBaseID(), pMountItem->GetName());

			lpctstr ptcFixString;
			switch (iFixCode)
			{
			default:	ptcFixString = "Undefined";														break;
			case 1:		ptcFixString = "Figurine UID was corrupted";									break;
			case 2:		ptcFixString = "Corrupted figurine UID corrupted and malformed mount item";		break;
			case 3:		ptcFixString = "Mount item was equipped by a char different from the rider";	break;
			case 4:		ptcFixString = "Malformed mount item";											break;
			case 5:		ptcFixString = "Mount item not linked to the mount char";						break;
			}
			g_Log.EventWarn(" Issue: %s.\n", ptcFixString);
		}
    }

    if (iFailureCode)
    {
		g_Log.EventError("Mount (UID=0%x, id=0%x '%s'): Can't auto-fix invalid mount item (mount item UID=ACTARG1=0%x)'\n",
			(dword)GetUID(), GetBaseID(), GetName(), (dword)m_atRidden.m_uidFigurine);

        lpctstr ptcFailureString;
        switch (iFailureCode)
        {
        default:    ptcFailureString = "Undefined";											break;
		case 1:     ptcFailureString = "Invalid mount item UID and owner has no equipped mount item";										break;
		case 2:     ptcFailureString = "Invalid mount item UID and mount item equipped by the owner is valid and linked to another mount";	break;
        case 3:     ptcFailureString = "Invalid owner";										break;
		case 4:		ptcFailureString = "Owner has no mount item in LAYER_HORSE";			break;
		case 5:		ptcFailureString = "ACTARG1 is not a mount item";						break;
		case 6:		ptcFailureString = "Both rider and owner had invalid mount item";		break;
		case 7:		ptcFailureString = "Owner has an invalid mount item in LAYER_HORSE";	break;
		case 8:		ptcFailureString = "Mount item equipped by the owner is valid and linked to another mount";								break;
        }
        g_Log.EventError(" Reason: %s.\n", ptcFailureString);
    }

    return Horse_GetMountItem();
}

// Gets my riding character, if i'm being mounted.
CChar* CChar::Horse_GetMountChar() const
{
    ADDTOCALLSTACK("CChar::Horse_GetMountChar");
    CItem* pItem = Horse_GetMountItem();
    if (pItem == nullptr)
        return nullptr;
    return dynamic_cast <CChar*>(pItem->GetTopLevelObj());
}

CChar * CChar::Horse_GetValidMountChar()
{
	ADDTOCALLSTACK("CChar::Horse_GetValidMountChar");
    CItem * pItem = Horse_GetValidMountItem();
	if ( pItem == nullptr )
		return nullptr;
	return dynamic_cast <CChar*>( pItem->GetTopLevelObj());
}

ITEMID_TYPE CChar::Horse_GetMountItemID() const
{
	tchar* ptcMountID = Str_GetTemp();
	snprintf(ptcMountID, STR_TEMPLENGTH, "mount_0x%x", GetDispID());
	lpctstr ptcMemoryID = g_Exp.m_VarDefs.GetKeyStr(ptcMountID);			// get the mount item defname from the mount_0x** defname

	CResourceID memoryRid = g_Cfg.ResourceGetID(RES_ITEMDEF, ptcMemoryID);
	return (ITEMID_TYPE)(memoryRid.GetResIndex());	// get the ID of the memory (mount item)
}

// Remove horse char and give player a horse item
// RETURN:
//  true = done mounting
//  false = we can't mount this
bool CChar::Horse_Mount(CChar *pHorse)
{
	ADDTOCALLSTACK("CChar::Horse_Mount");
    ASSERT(pHorse);
	ASSERT(pHorse->m_pNPC);

	if ( !CanTouch(pHorse) )
	{
		if ( pHorse->m_pNPC->m_bonded && pHorse->IsStatFlag(STATF_DEAD) )
			SysMessageDefault(DEFMSG_MSG_BONDED_DEAD_CANTMOUNT);
		else
			SysMessageDefault(DEFMSG_MSG_MOUNT_DIST);
		return false;
	}

	ITEMID_TYPE memoryId = pHorse->Horse_GetMountItemID();
	if ( memoryId <= ITEMID_NOTHING )
		return false;

	if ( !IsMountCapable() )
	{
		SysMessageDefault(DEFMSG_MSG_MOUNT_UNABLE);
		return false;
	}

	if ( !pHorse->NPC_IsOwnedBy(this) )
	{
		SysMessageDefault(DEFMSG_MSG_MOUNT_DONTOWN);
		return false;
	}

	if ( g_Cfg.m_iMountHeight )
	{
		if ( !IsVerticalSpace(GetTopPoint(), true) )	// is there space for the char + mount?
		{
			SysMessageDefault(DEFMSG_MSG_MOUNT_CEILING);
			return false;
		}
	}

	if ( IsTrigUsed(TRIGGER_MOUNT) )
	{
		CScriptTriggerArgs Args(pHorse);
   		if ( OnTrigger(CTRIG_Mount, this, &Args) == TRIGRET_RET_TRUE )
			return false;
	}

	// Create the figurine for the horse
	CItem * pMountItem = pHorse->Make_Figurine(GetUID(), memoryId);
	if ( !pMountItem)
		return false;

	// Set a new owner if it is not us (check first to prevent friends taking ownership)
	if ( !pHorse->NPC_IsOwnedBy(this, false) )
		pHorse->NPC_PetSetOwner(this);

	Horse_UnMount();					// unmount if already mounted
	// Use the figurine as a mount item
	pMountItem->SetType(IT_EQ_HORSE);
	pMountItem->SetTimeout(1);			// the first time we give it immediately a tick, then give the horse a tick everyone once in a while.
	LayerAdd(pMountItem, LAYER_HORSE);	// equip the horse item
	pHorse->StatFlag_Set(STATF_RIDDEN);
	pHorse->Skill_Start(NPCACT_RIDDEN);
	return true;
}

// Get off a horse (Remove horse item and spawn new horse)
bool CChar::Horse_UnMount()
{
	ADDTOCALLSTACK("CChar::Horse_UnMount");
	if ( !IsStatFlag(STATF_ONHORSE) || (IsStatFlag(STATF_STONE) && !IsPriv(PRIV_GM)) )
		return false;

	CItem * pMountItem = LayerFind(LAYER_HORSE);
	if (pMountItem == nullptr || pMountItem->IsDeleted() )
	{
		StatFlag_Clear(STATF_ONHORSE);	// flag got out of sync !
		return false;
	}

	CChar * pPet = pMountItem->m_itFigurine.m_UID.CharFind();
	if (pPet && IsTrigUsed(TRIGGER_DISMOUNT) && pPet->IsDisconnected() && !pPet->IsDeleted() ) // valid horse for trigger
	{
		CScriptTriggerArgs Args(pPet);
		if ( OnTrigger(CTRIG_Dismount, this, &Args) == TRIGRET_RET_TRUE )
			return false;
	}

	if (pMountItem->GetBaseID() == ITEMID_SHIP_PILOT)
	{
		CItem *pShip = pMountItem->m_uidLink.ItemFind();
        if (pShip)
		    pShip->m_itShip.m_Pilot.InitUID();

		SysMessageDefault(DEFMSG_SHIP_PILOT_OFF);
		pMountItem->Delete();
	}
	else
	{
		Use_Figurine(pMountItem, false);
		pMountItem->Delete();
		/*
		Actarg1 holds the UID of the mount item when the NPC is being ridden and as we can see in the Horse_GetMountItem method
		this actarg1 value is stored in the NPC not in the player.
		The commented  line below cleared the actarg1 of the rider instead of the NPC mount.
		*/
        //m_atRidden.m_uidFigurine.InitUID(); 
		if (pPet && !pPet->IsDeleted())
			pPet->m_atRidden.m_uidFigurine.InitUID(); //This clears the actarg1 of the NPC mount instead of the rider.
	}
	return true;
}

// A timer expired for an item we are carrying.
// Does it periodically do something ?
// Only for equipped items.
// RETURN:
//  false = delete it.
bool CChar::OnTickEquip( CItem * pItem )
{
	ADDTOCALLSTACK("CChar::OnTickEquip");
	if ( ! pItem )
		return false;
	switch ( pItem->GetEquipLayer())
	{
		case LAYER_FLAG_Wool:
			// This will regen the sheep it was sheered from.
			// Sheared sheep regen wool on a new day.
			if ( GetID() != CREID_SHEEP_SHORN )
				return false;

			// Is it a new day ? regen my wool.
			SetID( CREID_SHEEP );
			return false;

		case LAYER_FLAG_ClientLinger:
			// remove me from other clients screens.
			SetDisconnected();
			return false;

		case LAYER_SPECIAL:
			switch ( pItem->GetType())
			{
				case IT_EQ_SCRIPT:	// pure script.
					break;
				case IT_EQ_MEMORY_OBJ:
				{
					CItemMemory *pMemory = dynamic_cast<CItemMemory*>( pItem );
					if (pMemory)
						return Memory_OnTick(pMemory);

					return false;
				}
				default:
					break;
			}
			break;

		case LAYER_HORSE:
			// Give my horse a tick. (It is still in the game !)
			// NOTE: What if my horse dies (poisoned?)
			{
				CChar * pHorse = pItem->m_itFigurine.m_UID.CharFind();
				if ( pHorse == nullptr )
					return false;
				if ( pHorse != this )				//Some scripts can force mounts to have as 'mount' the rider itself (like old ethereal scripts)
					return pHorse->_OnTick();	    // if we call _OnTick again on them we'll have an infinite loop.
				pItem->SetTimeout( 1 );
				return true;
			}

		case LAYER_FLAG_Murders:
			// decay the murder count.
			{
				if ( ! m_pPlayer || m_pPlayer->m_wMurders <= 0  )
					return false;

				CScriptTriggerArgs	args;
				args.m_iN1 = m_pPlayer->m_wMurders - 1;
				args.m_iN2 = g_Cfg.m_iMurderDecayTime;

				if ( IsTrigUsed(TRIGGER_MURDERDECAY) )
				{
					OnTrigger(CTRIG_MurderDecay, this, &args);
					if ( args.m_iN1 < 0 ) args.m_iN1 = 0;
					if ( args.m_iN2 < 1 ) args.m_iN2 = g_Cfg.m_iMurderDecayTime;
				}

				m_pPlayer->m_wMurders = (word)(args.m_iN1);
				NotoSave_Update();
				if ( m_pPlayer->m_wMurders == 0 ) return false;
				pItem->SetTimeout(args.m_iN2);	// update it's decay time.
				return true;
			}

		default:
			break;
	}

	if ( pItem->IsType( IT_SPELL ))
	{
		return Spell_Equip_OnTick(pItem);
	}

	// Do not acquire the mutex lock here, or we'll have deadlocks in multiple situations
	return pItem->_OnTick();
}

// Leave the antidote in your body for a while.
// iSkill = 0-1000
bool CChar::SetPoisonCure( bool fExtra )
{
	ADDTOCALLSTACK("CChar::SetPoisonCure");

	CItem * pPoison = LayerFind( LAYER_FLAG_Poison );
	if ( pPoison )
		pPoison->Delete();

	if ( fExtra )
	{
		pPoison = LayerFind( LAYER_FLAG_Hallucination );
		if ( pPoison )
			pPoison->Delete();
		}

	UpdateModeFlag();
	return true;
}

// SPELL_Poison
// iSkill = 0-1000 = how bad the poison is.
// iHits = how much times the poison will hit. Irrelevant with MAGIFC_OSIFORMULAS enabled, because defaults will be used.
// Physical attack of poisoning.
bool CChar::SetPoison( int iSkill, int iHits, CChar * pCharSrc )
{
	ADDTOCALLSTACK("CChar::SetPoison");

	const CSpellDef *pSpellDef = g_Cfg.GetSpellDef(SPELL_Poison);
	if ( !pSpellDef )
		return false;

	// Release if paralyzed ?
	if ( !pSpellDef->IsSpellType(SPELLFLAG_NOUNPARALYZE) )
	{
		CItem *pParalyze = LayerFind(LAYER_SPELL_Paralyze);
		if ( pParalyze )
			pParalyze->Delete();
	}

	int64 iPoisonDuration = (1 + Calc_GetRandLLVal(2)) * TENTHS_PER_SEC;	//in TENTHS of second
	CItem* pPoison = Spell_Effect_Create(SPELL_Poison, LAYER_FLAG_Poison, iSkill, iPoisonDuration, pCharSrc, false);
	if ( !pPoison )
		return false;
	LayerAdd(pPoison, LAYER_FLAG_Poison);
	
	if (!IsSetMagicFlags(MAGICF_OSIFORMULAS))
	{
		//pPoison->m_itSpell.m_spellcharges has already been set by Spell_Effect_Create (and it's equal to iSkill)
		pPoison->m_itSpell.m_spellcharges = iHits;
	}
	else
	{
		// Get the poison level
		int iPoisonLevel = 0;

		int iDist = GetDist(pCharSrc);
		if (iDist <= UO_MAP_VIEW_SIZE_MAX)
		{
			if (iSkill >= 1000)		//Lethal-Deadly
				iPoisonLevel = 3 + !bool(Calc_GetRandVal(10));
			else if (iSkill > 850)	//Greater
				iPoisonLevel = 2;
			else if (iSkill > 650)	//Standard
				iPoisonLevel = 1;
			else					//Lesser
				iPoisonLevel = 0;
			if (iDist >= 4)
			{
				iPoisonLevel -= (iDist / 2);
				if (iPoisonLevel < 0)
					iPoisonLevel = 0;
			}
		}
		pPoison->m_itSpell.m_spelllevel = (word)iPoisonLevel;	// Overwrite the spell level

		switch (iPoisonLevel)
		{
			case 4:		pPoison->m_itSpell.m_spellcharges = 8; break;
			case 3:		pPoison->m_itSpell.m_spellcharges = 6; break;
			case 2:		pPoison->m_itSpell.m_spellcharges = 6; break;
			case 1:		pPoison->m_itSpell.m_spellcharges = 3; break;
			default:
			case 0:		pPoison->m_itSpell.m_spellcharges = 3; break;
		}

		if (IsAosFlagEnabled(FEATURE_AOS_UPDATE_B))
		{
			CItem* pEvilOmen = LayerFind(LAYER_SPELL_Evil_Omen);
			if (pEvilOmen && !g_Cfg.GetSpellDef(SPELL_Evil_Omen)->IsSpellType(SPELLFLAG_SCRIPTED))
			{
				++pPoison->m_itSpell.m_spelllevel;	// Effect 2: next poison will have one additional level of poison, this makes sense only with MAGICF_OSIFORMULAS enabled.
				pEvilOmen->Delete();
			}
		}
	}

	

	CClient *pClient = GetClientActive();
	if ( pClient && IsSetOF(OF_Buffs) )
	{
		pClient->removeBuff(BI_POISON);
		pClient->addBuff(BI_POISON, 1017383, 1070722, (word)(pPoison->m_itSpell.m_spellcharges));
	}

	SysMessageDefault(DEFMSG_JUST_BEEN_POISONED);
	StatFlag_Set(STATF_POISONED);
	UpdateModeFlag();
	return true;
}

// Not sleeping anymore.
void CChar::Wake()
{
	ADDTOCALLSTACK("CChar::Wake");
	if (!IsStatFlag(STATF_SLEEPING))
		return;

	CItemCorpse *pCorpse = FindMyCorpse(true);
	if (pCorpse == nullptr)
	{
		Stat_SetVal(STAT_STR, 0);		// death
		return;
	}

	RaiseCorpse(pCorpse);
	StatFlag_Clear(STATF_SLEEPING);
	UpdateMode();
}

// Sleep
void CChar::SleepStart( bool fFrontFall )
{
	ADDTOCALLSTACK("CChar::SleepStart");
	if (IsStatFlag(STATF_DEAD|STATF_SLEEPING|STATF_POLYMORPH))
		return;

	CItemCorpse *pCorpse = MakeCorpse(fFrontFall);
	if (pCorpse == nullptr)
	{
		SysMessageDefault(DEFMSG_MSG_CANTSLEEP);
		return;
	}

	// Play death animation (fall on ground)
	UpdateCanSee(new PacketDeath(this, pCorpse, fFrontFall));
    pCorpse->Update();

	SetID(_iPrev_id);
	StatFlag_Set(STATF_SLEEPING);
	StatFlag_Clear(STATF_HIDDEN);
	UpdateMode();
}

// We died, calling @Death, removing trade windows.
// Give credit to my killers ( @Kill ).
// Cleaning myself (dispel, cure, dismounting ...).
// Creating the corpse ( MakeCorpse() ).
// Removing myself from view, generating Death packets.
// RETURN:
//		true = successfully died
//		false = something went wrong? i'm an NPC, just delete (excepting BONDED ones).
bool CChar::Death()
{
	ADDTOCALLSTACK("CChar::Death");

	if ( IsStatFlag(STATF_DEAD|STATF_INVUL) )
		return true;

	if ( IsTrigUsed(TRIGGER_DEATH) )
	{
		if ( OnTrigger(CTRIG_Death, this) == TRIGRET_RET_TRUE )
			return true;
	}

	// Look through memories of who I was fighting (make sure they knew they where fighting me)
	for (CSObjContRec* pObjRec : GetIterationSafeContReverse())
	{
		CItem* pItem = static_cast<CItem*>(pObjRec);
		if ( pItem->IsType(IT_EQ_TRADE_WINDOW) )
		{
			CItemContainer *pCont = dynamic_cast<CItemContainer *>(pItem);
			if ( pCont )
			{
				pCont->Trade_Delete();
				continue;
			}
		}

		// Remove every memory, with some exceptions
		if ( pItem->IsType(IT_EQ_MEMORY_OBJ) )
			Memory_ClearTypes( static_cast<CItemMemory *>(pItem), (MEMORY_FIGHT | MEMORY_HARMEDBY) );
	}

	// Give credit for the kill to my attacker(s)
	int iKillers = 0;
	CChar * pKiller = nullptr;
	tchar * pszKillStr = Str_GetTemp();
	int iKillStrLen = snprintf( pszKillStr, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_MSG_KILLED_BY), (m_pPlayer)? 'P':'N', GetNameWithoutIncognito() );
	for ( size_t count = 0; count < m_lastAttackers.size(); ++count )
	{
		pKiller = CUID::CharFindFromUID(m_lastAttackers[count].charUID);
		if ( pKiller && (m_lastAttackers[count].amountDone > 0) )
		{
			if ( IsTrigUsed(TRIGGER_KILL) )
			{
				CScriptTriggerArgs args(this);
				args.m_iN1 = GetAttackersCount();
				if ( pKiller->OnTrigger(CTRIG_Kill, this, &args) == TRIGRET_RET_TRUE )
					continue;
			}

			pKiller->Noto_Kill( this, GetAttackersCount() );

			iKillStrLen += snprintf(
				pszKillStr + iKillStrLen, STR_TEMPLENGTH - iKillStrLen,
				"%s%c'%s'.",
				iKillers ? ", " : "", (pKiller->m_pPlayer) ? 'P':'N', pKiller->GetNameWithoutIncognito() );

			++iKillers;
		}
	}

	// Record the kill event for posterity
	if ( !iKillers )
		iKillStrLen += snprintf( pszKillStr + iKillStrLen, STR_TEMPLENGTH - iKillStrLen, "accident." );
	if ( m_pPlayer )
		g_Log.Event( LOGL_EVENT|LOGM_KILLS, "%s\n", pszKillStr );
	if ( m_pParty )
		m_pParty->SysMessageAll( pszKillStr );

	Reveal();
	SoundChar(CRESND_DIE);
	StatFlag_Set(STATF_DEAD);
	StatFlag_Clear(STATF_STONE|STATF_FREEZE|STATF_HIDDEN|STATF_SLEEPING|STATF_HOVERING);
	SetPoisonCure(true);
	Skill_Cleanup();
	Spell_Dispel(100);		// get rid of all spell effects (moved here to prevent double @Destroy trigger)

	if ( m_pPlayer )		// if I'm NPC then my mount goes with me
		Horse_UnMount();

    if ( IsTrigUsed(TRIGGER_CREATELOOT) )
    {
        //OnTrigger(CTRIG_CreateLoot, this);
        ReadScriptReducedTrig(Char_GetDef(), CTRIG_CreateLoot, false);
    }

	// Create the corpse item
	bool fFrontFall = Calc_GetRandVal(2);
	CItemCorpse * pCorpse = MakeCorpse(fFrontFall);
	if ( pCorpse )
	{
		if ( IsTrigUsed(TRIGGER_DEATHCORPSE) )
		{
			CScriptTriggerArgs Args(pCorpse);
			OnTrigger(CTRIG_DeathCorpse, this, &Args);
		}
	}
	m_lastAttackers.clear();	// clear list of attackers

	// Play death animation (fall on ground)
	UpdateCanSee(new PacketDeath(this, pCorpse, fFrontFall), m_pClient);

	if ( m_pNPC )
	{
		if ( m_pNPC->m_bonded )
		{
			m_CanMask |= CAN_C_GHOST;
			UpdateMode(nullptr, true);
			return true;
		}

		if ( pCorpse )
			pCorpse->m_uidLink.InitUID();

		NPC_PetClearOwners();
		return false;	// delete the NPC
	}

	if ( m_pPlayer )
	{
        llong iDelta = m_exp / 10;
		ChangeExperience(- maximum(1, iDelta), pKiller);
		if ( !(m_TagDefs.GetKeyNum("DEATHFLAGS") & DEATH_NOFAMECHANGE) )
			Noto_Fame( -GetFame()/10 );

		lpctstr pszGhostName = nullptr;
		const CCharBase *pCharDefPrev = CCharBase::FindCharBase( _iPrev_id );
        const bool fFemale = pCharDefPrev && pCharDefPrev->IsFemale();
		switch ( _iPrev_id )
		{
			case CREID_GARGMAN:
			case CREID_GARGWOMAN:
				pszGhostName = ( fFemale ? "c_garg_ghost_woman" : "c_garg_ghost_man" );
				break;
			case CREID_ELFMAN:
			case CREID_ELFWOMAN:
				pszGhostName = ( fFemale ? "c_elf_ghost_woman" : "c_elf_ghost_man" );
				break;
			default:
				pszGhostName = ( fFemale ? "c_ghost_woman" : "c_ghost_man" );
				break;
		}
		ASSERT(pszGhostName != nullptr);

		if ( !IsStatFlag(STATF_WAR) )
			StatFlag_Set(STATF_INSUBSTANTIAL);	// manifest war mode for ghosts

		++m_pPlayer->m_wDeaths;

		SetHue( HUE_DEFAULT );	// get all pale
		SetID( (CREID_TYPE)(g_Cfg.ResourceGetIndexType( RES_CHARDEF, pszGhostName )) );
		LayerAdd( CItem::CreateScript( ITEMID_DEATHSHROUD, this ) );

        CClient * pClient = GetClientActive();
		if ( pClient )
		{
            if (g_Cfg.m_iPacketDeathAnimation)
            {
                // OSI uses PacketDeathMenu to update client screen on death.
                // If the user disable this packet, it must be updated using addPlayerUpdate()

                // Display death animation to client ("You are dead")
                new PacketDeathMenu(pClient, PacketDeathMenu::Dead);
            }
            else
            {
                pClient->addPlayerUpdate();
            }
            pClient->addPlayerWarMode();
            pClient->addSeason(SEASON_Desolate);
            pClient->addMapWaypoint(pCorpse, MAPWAYPOINT_Corpse);		// add corpse map waypoint on enhanced clients

            CItem *pPack = LayerFind(LAYER_PACK);
            if ( pPack )
            {
                pPack->RemoveFromView();
                pPack->Update();
            }

		    // Remove the characters which I can't see as dead from the screen
            if (g_Cfg.m_fDeadCannotSeeLiving)
            {
                CWorldSearch AreaChars(GetTopPoint(), UO_MAP_VIEW_SIZE_MAX);
                AreaChars.SetSearchSquare(true);
                for (;;)
                {
                    CChar *pChar = AreaChars.GetChar();
                    if (!pChar)
                        break;
                    if (!CanSeeAsDead(pChar))
                        pClient->addObjectRemove(pChar);
                }
            }

		}
	}
	return true;
}

// Check if we are held in place.
// RETURN: true = held in place.
bool CChar::OnFreezeCheck() const
{
	ADDTOCALLSTACK("CChar::OnFreezeCheck");

	if ( IsStatFlag(STATF_FREEZE|STATF_STONE) && !IsPriv(PRIV_GM) )
		return true;
	if ( GetKeyNum("NoMoveTill") > (CWorldGameTime::GetCurrentTime().GetTimeRaw() / MSECS_PER_TENTH)) // in tenths of second.
		return true;

	if ( m_pPlayer )
	{
		if ( m_pPlayer->m_speedMode & 0x04 )	// speed mode '4' prevents movement
			return true;

		if ( IsSetMagicFlags(MAGICF_FREEZEONCAST) && g_Cfg.IsSkillFlag(m_Act_SkillCurrent, SKF_MAGIC) )		// casting magic spells
		{
			const CSpellDef *pSpellDef = g_Cfg.GetSpellDef(m_atMagery.m_iSpell);
			if ( pSpellDef && !pSpellDef->IsSpellType(SPELLFLAG_NOFREEZEONCAST) )
				return true;
		}
	}

	return false;
}

bool CChar::IsStuck(bool fFreezeCheck)
{
    CPointMap pt = GetTopPoint();
    if ( fFreezeCheck && OnFreezeCheck() )
        return true;
    return !( CanMoveWalkTo(pt, true, true, DIR_N) || CanMoveWalkTo(pt, true, true, DIR_E) || CanMoveWalkTo(pt, true, true, DIR_S) || CanMoveWalkTo(pt, true, true, DIR_W) );
}

// Flip around
void CChar::Flip()
{
	ADDTOCALLSTACK("CChar::Flip");
	UpdateDir( GetDirTurn( m_dirFace, 1 ));
}

// For both players and NPC's
// Walk towards this point as best we can.
// Affect stamina as if we WILL move !
// RETURN:
//  ptDst.m_z = the new z
//  nullptr = failed to walk here.
CRegion * CChar::CanMoveWalkTo( CPointMap & ptDst, bool fCheckChars, bool fCheckOnly, DIR_TYPE dir, bool fPathFinding )
{
	ADDTOCALLSTACK("CChar::CanMoveWalkTo");

	if ( Can(CAN_C_NONMOVER|CAN_C_STATUE) || IsStatFlag(STATF_FREEZE|STATF_STONE) )
		return nullptr;

	int iWeightLoadPercent = GetWeightLoadPercent(GetTotalWeight());
	if ( !fCheckOnly )
	{
		if ( OnFreezeCheck() )
		{
			SysMessageDefault(DEFMSG_MSG_FROZEN);
			return nullptr;
		}

		if ( (Stat_GetVal(STAT_DEX) <= 0) && (!IsStatFlag(STATF_DEAD)) )
		{
			SysMessageDefault(DEFMSG_MSG_FATIGUE);
			return nullptr;
		}

		if ( iWeightLoadPercent > 200 )
		{
			SysMessageDefault(DEFMSG_MSG_OVERLOAD);
			return nullptr;
		}
	}

	CClient *pClient = GetClientActive();
	if ( pClient && pClient->m_pHouseDesign )
	{
		if ( pClient->m_pHouseDesign->GetDesignArea().IsInside2d(ptDst) )
		{
			ptDst.m_z = GetTopZ();
			return ptDst.GetRegion(REGION_TYPE_MULTI|REGION_TYPE_AREA);
		}
		return nullptr;
	}

	// ok to go here ? physical blocking objects ?
	dword dwBlockFlags = 0;
	height_t ClimbHeight = 0;
	CRegion *pArea = nullptr;

	EXC_TRY("CanMoveWalkTo");

	EXC_SET_BLOCK("Check Valid Move");
	pArea = CheckValidMove(ptDst, &dwBlockFlags, dir, &ClimbHeight, fPathFinding);
	if ( !pArea )
	{
		if (g_Cfg.m_iDebugFlags & DEBUGF_WALK)
            g_Log.EventWarn("CheckValidMove failed\n");
		return nullptr;
	}

	EXC_SET_BLOCK("NPC's will");
	if ( !fCheckOnly && m_pNPC && !NPC_CheckWalkHere(ptDst, pArea) )	// does the NPC want to walk here?
		return nullptr;

	EXC_SET_BLOCK("Creature bumping");
	ushort uiStamReq = 0;
	if ( fCheckChars && !IsStatFlag(STATF_DEAD|STATF_SLEEPING|STATF_INSUBSTANTIAL) )
	{
		CItem *pPoly = LayerFind(LAYER_SPELL_Polymorph);
		CWorldSearch AreaChars(ptDst);
		for (;;)
		{
			CChar *pChar = AreaChars.GetChar();
			if (!pChar)
				break;
            if (pChar->Can(CAN_C_STATUE))
                return nullptr; // can't walk over a statue
			if ( (pChar == this) || (abs(pChar->GetTopZ() - ptDst.m_z) > 5) || (pChar->IsStatFlag(STATF_INSUBSTANTIAL)) )
				continue;
			if ( m_pNPC && pChar->m_pNPC )	// NPCs can't walk over another NPC
				return nullptr;

			uiStamReq = 10;		// Stam consume for push the char. OSI seem to be 10% and not a fix 10
			if ( IsPriv(PRIV_GM) || pChar->IsStatFlag(STATF_DEAD) || (pChar->IsStatFlag(STATF_INVISIBLE|STATF_HIDDEN) && !(g_Cfg.m_iRevealFlags & REVEALF_OSILIKEPERSONALSPACE)) )
				uiStamReq = 0;	// On SPHERE, need 0 stam to reveal someone 
			else if ( (pPoly && pPoly->m_itSpell.m_spell == SPELL_Wraith_Form) && (GetTopMap() == 0) )		// chars under Wraith Form effect can always walk through chars in Felucca
				uiStamReq = 0;

			TRIGRET_TYPE iRet = TRIGRET_RET_DEFAULT;
			if ( IsTrigUsed(TRIGGER_PERSONALSPACE) )
			{
				CScriptTriggerArgs Args(uiStamReq);
				iRet = pChar->OnTrigger(CTRIG_PersonalSpace, this, &Args);
				if ( iRet == TRIGRET_RET_TRUE )
					return nullptr;
                uiStamReq = (ushort)(Args.m_iN1);
			}


			if ( (uiStamReq > 0) && (Stat_GetVal(STAT_DEX) < Stat_GetMaxAdjusted(STAT_DEX)) )
				return nullptr;

			tchar *pszMsg = Str_GetTemp();
			if ( Stat_GetVal(STAT_DEX) < uiStamReq )		// check if we have enough stamina to push the char
			{
				snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_MSG_CANTPUSH), pChar->GetName());
				SysMessage(pszMsg);
				return nullptr;
			}
			
			else if (pChar->IsStatFlag(STATF_INVISIBLE | STATF_HIDDEN) ) {
				if ((g_Cfg.m_iRevealFlags & REVEALF_OSILIKEPERSONALSPACE))
					// OSILIKEPERSONALSPACE flag block the reveal but DEFMSG_HIDING_STUMBLE_OSILIKE is send. To avoid it, simply use return 1 in @PERSONALSPACE 
					snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_HIDING_STUMBLE_OSILIKE));
				else 
				{
					snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_HIDING_STUMBLE), pChar->GetName());
					if (!fPathFinding) // When NPC use pathfinding(NPC_AI_PATH) to calculate their destination, char should not be reveal
						pChar->Reveal(STATF_INVISIBLE | STATF_HIDDEN);
				}
			}

			else if ( pChar->IsStatFlag(STATF_SLEEPING) )
				snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_MSG_STEPON_BODY), pChar->GetName());
			else
				snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_MSG_PUSH), pChar->GetName());
			
			if ( iRet != TRIGRET_RET_FALSE )
				SysMessage(pszMsg);

			break;
		}
	}

	if ( !fCheckOnly )
	{
		EXC_SET_BLOCK("Stamina penalty");
		// Chance to drop more stamina if running or overloaded
		CVarDefCont *pVal = GetKey("OVERRIDE.RUNNINGPENALTY", true);
		if ( IsStatFlag(STATF_FLY|STATF_HOVERING) )
			iWeightLoadPercent += pVal ? (int)(pVal->GetValNum()) : g_Cfg.m_iStamRunningPenalty;

		pVal = GetKey("OVERRIDE.STAMINALOSSATWEIGHT", true);
		int iChanceForStamLoss = Calc_GetSCurve(iWeightLoadPercent - (pVal ? (int)(pVal->GetValNum()) : g_Cfg.m_iStaminaLossAtWeight), 10);
		if ( iChanceForStamLoss > Calc_GetRandVal(1000) )
			uiStamReq += 1;

		if ( uiStamReq )
			UpdateStatVal(STAT_DEX, -uiStamReq);

		StatFlag_Mod(STATF_INDOORS, (dwBlockFlags & CAN_I_ROOF) || pArea->IsFlag(REGION_FLAG_UNDERGROUND));
		m_zClimbHeight = (dwBlockFlags & CAN_I_CLIMB) ? ClimbHeight : 0;
	}

	EXC_CATCH;
	return pArea;
}

// Are we going to reveal ourselves by moving ?
void CChar::CheckRevealOnMove()
{
	ADDTOCALLSTACK("CChar::CheckRevealOnMove");

	if ( !IsStatFlag(STATF_INVISIBLE|STATF_HIDDEN|STATF_SLEEPING) )
		return;

	if ( IsTrigUsed(TRIGGER_STEPSTEALTH) )
		OnTrigger(CTRIG_StepStealth, this);

	m_StepStealth -= IsStatFlag(STATF_FLY|STATF_HOVERING) ? 2 : 1;
	if ( m_StepStealth <= 0 )
		Reveal();
}

// We are at this location. What will happen?
// This function is called at every second on ALL chars
// (even walking or not), so avoid heavy codes here.
// RETURN:
//	true = we can move there
//	false = we can't move there
//	default = we teleported
TRIGRET_TYPE CChar::CheckLocation( bool fStanding )
{
	ADDTOCALLSTACK("CChar::CheckLocation");

	CClient *pClient = GetClientActive();
	if ( pClient && pClient->m_pHouseDesign )
	{
		// Stepping on items doesn't trigger anything whilst in design mode
		if ( pClient->m_pHouseDesign->GetDesignArea().IsInside2d(GetTopPoint()) )
			return TRIGRET_RET_TRUE;

		pClient->m_pHouseDesign->EndCustomize(true);
	}

	if ( !fStanding )
	{
		SKILL_TYPE iSkillActive	= Skill_GetActive();
		if ( g_Cfg.IsSkillFlag(iSkillActive, SKF_IMMOBILE) )
			Skill_Fail(false);
		else if ( g_Cfg.IsSkillFlag(iSkillActive, SKF_FIGHT) && g_Cfg.IsSkillFlag(iSkillActive, SKF_RANGED) && !IsSetCombatFlags(COMBAT_ARCHERYCANMOVE) && !IsStatFlag(STATF_ARCHERCANMOVE) )
		{
			// Keep timer active holding the swing action until the char stops moving
			m_atFight.m_iWarSwingState = WAR_SWING_EQUIPPING;
			_SetTimeoutD(1);
		}

		// This could get REALLY EXPENSIVE !
		if ( IsTrigUsed(TRIGGER_STEP) )
		{
			if ( m_pArea->OnRegionTrigger( this, RTRIG_STEP ) == TRIGRET_RET_TRUE )
				return TRIGRET_RET_FALSE;

			CRegion *pRoom = GetTopPoint().GetRegion(REGION_TYPE_ROOM);
			if ( pRoom && pRoom->OnRegionTrigger( this, RTRIG_STEP ) == TRIGRET_RET_TRUE )
				return TRIGRET_RET_FALSE;
		}
	}

	bool fStepCancel = false;
	bool fSpellHit = false;
	CWorldSearch AreaItems( GetTopPoint() );
	for (;;)
	{
		CItem *pItem = AreaItems.GetItem();
		if ( !pItem )
			break;

		int zdiff = pItem->GetTopZ() - GetTopZ();
		int	height = pItem->Item_GetDef()->GetHeight();
		if ( height < 3 )
			height = 3;

		if ( (zdiff > height) || (zdiff < -3) )
			continue;
		if ( IsTrigUsed(TRIGGER_STEP) || IsTrigUsed(TRIGGER_ITEMSTEP) )
		{
			CScriptTriggerArgs Args(fStanding ? 1 : 0);
			TRIGRET_TYPE iRet = pItem->OnTrigger(ITRIG_STEP, this, &Args);
			if ( iRet == TRIGRET_RET_TRUE )		// block walk
			{
				fStepCancel = true;
				continue;
			}
			if ( iRet == TRIGRET_RET_HALFBAKED )	// allow walk, skipping hardcoded checks below
				continue;
		}

		switch ( pItem->GetType() )
		{
			case IT_WEB:
				if ( fStanding )
					continue;
				if ( Use_Item_Web(pItem) )	// we got stuck in a spider web
					return TRIGRET_RET_TRUE;
				continue;
			case IT_FIRE:
				{
					int iSkillLevel = pItem->m_itSpell.m_spelllevel;	// heat level (0-1000)
					iSkillLevel = Calc_GetRandVal2(iSkillLevel/2, iSkillLevel);
					if ( IsStatFlag(STATF_FLY) )
						iSkillLevel /= 2;

					int iDmg = OnTakeDamage( g_Cfg.GetSpellEffect(SPELL_Fire_Field, iSkillLevel), nullptr, DAMAGE_FIRE|DAMAGE_GENERAL, 0, 100, 0, 0, 0 );
                    if (iDmg > 0)
                    {
                        Sound(0x15f);	// fire noise
                        if ( m_pNPC && fStanding )
                        {
                            m_Act_p.Move((DIR_TYPE)(Calc_GetRandVal(DIR_QTY)));
                            NPC_WalkToPoint(true);		// run away from the threat
                        }
                    }
				}
				continue;
			case IT_SPELL:
				// Workaround: only hit 1 spell on each loop. If we hit all spells (eg: multiple field spells)
				// it will allow weird exploits like cast many Fire Fields on the same spot to take more damage,
				// or Paralyze Field + Fire Field to make the target get stuck forever being damaged with no way
				// to get out of the field, since the damage won't allow cast any spell and the Paralyze Field
				// will immediately paralyze again with 0ms delay at each damage tick.
				// On OSI if the player cast multiple fields on the same tile, it will remove the previous field
				// tile that got overlapped. But Sphere doesn't use this method, so this workaround is needed.
				if (fSpellHit)
                    continue;

                fSpellHit = OnSpellEffect((SPELL_TYPE)(RES_GET_INDEX(pItem->m_itSpell.m_spell)),
                    pItem->m_uidLink.CharFind(), pItem->m_itSpell.m_spelllevel, pItem);
                if (fSpellHit && m_pNPC && fStanding)
                {
                    m_Act_p.Move((DIR_TYPE)(Calc_GetRandVal(DIR_QTY)));
                    NPC_WalkToPoint(true);		// run away from the threat
                }
				continue;
			case IT_TRAP:
			case IT_TRAP_ACTIVE:
            {
                int iDmg = OnTakeDamage( pItem->Use_Trap(), nullptr, DAMAGE_HIT_BLUNT|DAMAGE_GENERAL );
                if ( (iDmg > 0) && m_pNPC && fStanding )
                {
                    m_Act_p.Move((DIR_TYPE)(Calc_GetRandVal(DIR_QTY)));
                    NPC_WalkToPoint(true);		// run away from the threat
                }
                continue;
            }
			case IT_SWITCH:
				if ( pItem->m_itSwitch.m_wStep )
					Use_Item(pItem);
				continue;
			case IT_MOONGATE:
			case IT_TELEPAD:
				if ( fStanding )
					continue;
				Use_MoonGate(pItem);
				return TRIGRET_RET_DEFAULT;
			case IT_SHIP_PLANK:
			case IT_ROPE:
				if ( !fStanding && !IsStatFlag(STATF_HOVERING) )
				{
					// Check if we can go out of the ship (in the same direction of plank)
                    //bool fFromShip = (nullptr != GetTopSector()->GetRegion(GetTopPoint(), REGION_TYPE_SHIP)); // always true
					if ( MoveToValidSpot(m_dirFace, g_Cfg.m_iMaxShipPlankTeleport, 1, true) )
					{
						//pItem->SetTimeoutS(5);	// autoclose the plank behind us
						return TRIGRET_RET_TRUE;
					}
				}
				continue;
			default:
				continue;
		}
	}

	if (fStepCancel)
		return TRIGRET_RET_FALSE;
    if (fStanding)
        return TRIGRET_RET_TRUE;

	// Check the map teleporters in this CSector (if any)
	const CPointMap &pt = GetTopPoint();
	CSector *pSector = pt.GetSector();
	if ( !pSector )
		return TRIGRET_RET_FALSE;

	const CTeleport *pTeleport = pSector->GetTeleport(pt);
	if ( !pTeleport )
		return TRIGRET_RET_TRUE;

	if ( m_pNPC )
	{
		if ( !pTeleport->_fNpc )
			return TRIGRET_RET_FALSE;

		if ( m_pNPC->m_Brain == NPCBRAIN_GUARD )
		{
			// Guards won't gate into unguarded areas.
			const CRegionWorld *pArea = dynamic_cast<CRegionWorld*>(pTeleport->_ptDst.GetRegion(REGION_TYPE_MULTI|REGION_TYPE_AREA));
			if ( !pArea || (!pArea->IsGuarded() && !IsSetOF(OF_GuardOutsideGuardedArea)) )
				return TRIGRET_RET_FALSE;
		}
		if ( Noto_IsCriminal() )
		{
			// wont teleport to guarded areas.
			const CRegionWorld *pArea = dynamic_cast<CRegionWorld*>(pTeleport->_ptDst.GetRegion(REGION_TYPE_MULTI|REGION_TYPE_AREA));
			if ( !pArea || pArea->IsGuarded() )
				return TRIGRET_RET_FALSE;
		}
	}
	Spell_Teleport(pTeleport->_ptDst, true, false, false);
	return TRIGRET_RET_DEFAULT;
}

// Moving to a new region. or logging out (not in any region)
// pNewArea == nullptr = we are logging out.
// RETURN:
//  false = do not allow in this area.
bool CChar::MoveToRegion( CRegionWorld * pNewArea, bool fAllowReject )
{
	ADDTOCALLSTACK("CChar::MoveToRegion");
	if ( m_pArea == pNewArea )
		return true;

	if ( ! g_Serv.IsLoading())
	{
		if ( fAllowReject && IsPriv( PRIV_GM ))
		{
			fAllowReject = false;
		}

		// Leaving region trigger. (may not be allowed to leave ?)
		if ( m_pArea )
		{
			if ( IsTrigUsed(TRIGGER_EXIT) )
			{
				if ( m_pArea->OnRegionTrigger( this, RTRIG_EXIT ) == TRIGRET_RET_TRUE )
				{
					if ( pNewArea && fAllowReject )
						return false;
				}
			}

			if ( IsTrigUsed(TRIGGER_REGIONLEAVE) )
			{
				CScriptTriggerArgs Args(m_pArea);
				if ( OnTrigger(CTRIG_RegionLeave, this, & Args) == TRIGRET_RET_TRUE )
				{
					if ( pNewArea && fAllowReject )
						return false;
				}
			}
		}

		if ( IsClientActive() && pNewArea )
		{
			if ( pNewArea->IsFlag(REGION_FLAG_ANNOUNCE) && !pNewArea->IsInside2d( GetTopPoint()) )	// new area.
			{
				CVarDefContStr * pVarStr = dynamic_cast <CVarDefContStr *>( pNewArea->m_TagDefs.GetKey("ANNOUNCEMENT"));
				SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_MSG_REGION_ENTER), (pVarStr != nullptr) ? pVarStr->GetValStr() : pNewArea->GetName());
			}

			// Is it guarded / safe / non-pvp?
			else if ( m_pArea && !IsStatFlag(STATF_DEAD) )
			{
				const bool fRedNew = ( pNewArea->m_TagDefs.GetKeyNum("RED") != 0 );
				const bool fRedOld = ( m_pArea->m_TagDefs.GetKeyNum("RED") != 0 );
				if ( pNewArea->IsGuarded() != m_pArea->IsGuarded() )
				{
					if ( pNewArea->IsGuarded() )	// now under the protection
					{
						CVarDefContStr *pVarStr = dynamic_cast<CVarDefContStr *>(pNewArea->m_TagDefs.GetKey("GUARDOWNER"));
						SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_MSG_REGION_GUARDS_1), (pVarStr != nullptr) ? pVarStr->GetValStr() : g_Cfg.GetDefaultMsg(DEFMSG_MSG_REGION_GUARD_ART));
					}
					else							// have left the protection
					{
						CVarDefContStr *pVarStr = dynamic_cast<CVarDefContStr *>(m_pArea->m_TagDefs.GetKey("GUARDOWNER"));
						SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_MSG_REGION_GUARDS_2), (pVarStr != nullptr) ? pVarStr->GetValStr() : g_Cfg.GetDefaultMsg(DEFMSG_MSG_REGION_GUARD_ART));
					}
				}
				if ( fRedNew != fRedOld )
					SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_MSG_REGION_REDDEF), g_Cfg.GetDefaultMsg(fRedNew ? DEFMSG_MSG_REGION_REDENTER : DEFMSG_MSG_REGION_REDLEFT));
				/*else if ( fRedNew && ( fRedNew == fRedOld ))
				{
					SysMessage("You are still in the red region.");
				}*/
				if ( pNewArea->IsFlag(REGION_FLAG_NO_PVP) != m_pArea->IsFlag(REGION_FLAG_NO_PVP))
					SysMessageDefault(( pNewArea->IsFlag(REGION_FLAG_NO_PVP)) ? DEFMSG_MSG_REGION_PVPSAFE : DEFMSG_MSG_REGION_PVPNOT );
				if ( pNewArea->IsFlag(REGION_FLAG_SAFE) != m_pArea->IsFlag(REGION_FLAG_SAFE) )
					SysMessageDefault((pNewArea->IsFlag(REGION_FLAG_SAFE)) ? DEFMSG_MSG_REGION_SAFETYGET : DEFMSG_MSG_REGION_SAFETYLOSE);
			}
		}

		// Entering region trigger.
		if ( pNewArea )
		{
			if ( IsTrigUsed(TRIGGER_ENTER) )
			{
				if ( pNewArea->OnRegionTrigger( this, RTRIG_ENTER ) == TRIGRET_RET_TRUE )
				{
					if ( m_pArea && fAllowReject )
						return false;
				}
			}
			if ( IsTrigUsed(TRIGGER_REGIONENTER) )
			{
				CScriptTriggerArgs Args(pNewArea);
				if ( OnTrigger(CTRIG_RegionEnter, this, & Args) == TRIGRET_RET_TRUE )
				{
					if ( m_pArea && fAllowReject )
						return false;
				}
			}
		}
	}

	m_pArea = pNewArea;
	return true;
}

// Moving to a new room.
// RETURN:
// false = do not allow in this room.
bool CChar::MoveToRoom( CRegion * pNewRoom, bool fAllowReject)
{
	ADDTOCALLSTACK("CChar::MoveToRoom");

	if ( m_pRoom == pNewRoom )
		return true;

	if ( ! g_Serv.IsLoading())
	{
		if ( fAllowReject && IsPriv( PRIV_GM ))
		{
			fAllowReject = false;
		}

		// Leaving room trigger. (may not be allowed to leave ?)
		if ( m_pRoom )
		{
			if ( IsTrigUsed(TRIGGER_EXIT) )
			{
				if ( m_pRoom->OnRegionTrigger( this, RTRIG_EXIT ) == TRIGRET_RET_TRUE )
				{
					if (fAllowReject )
						return false;
				}
			}

			if ( IsTrigUsed(TRIGGER_REGIONLEAVE) )
			{
				CScriptTriggerArgs Args(m_pRoom);
				if ( OnTrigger(CTRIG_RegionLeave, this, & Args) == TRIGRET_RET_TRUE )
				{
					if (fAllowReject )
						return false;
				}
			}
		}

		// Entering room trigger
		if ( pNewRoom )
		{
			if ( IsTrigUsed(TRIGGER_ENTER) )
			{
				if ( pNewRoom->OnRegionTrigger( this, RTRIG_ENTER ) == TRIGRET_RET_TRUE )
				{
					if (fAllowReject )
						return false;
				}
			}
			if ( IsTrigUsed(TRIGGER_REGIONENTER) )
			{
				CScriptTriggerArgs Args(pNewRoom);
				if ( OnTrigger(CTRIG_RegionEnter, this, & Args) == TRIGRET_RET_TRUE )
				{
					if (fAllowReject )
						return false;
				}
			}
		}
	}

	m_pRoom = pNewRoom;
	return true;
}

bool CChar::MoveToRegionReTest( dword dwType )
{
	return MoveToRegion( dynamic_cast <CRegionWorld *>( GetTopPoint().GetRegion( dwType )), false);
}

// Same as MoveTo
// This could be us just taking a step or being teleported.
// Low level: DOES NOT UPDATE DISPLAYS or container flags. (may be offline)
// This does not check for gravity.
bool CChar::MoveToChar(const CPointMap& pt, bool fStanding, bool fCheckLocation, bool fForceFix, bool fAllowReject)
{
	ADDTOCALLSTACK("CChar::MoveToChar");

	if ( !pt.IsValidPoint() )
		return false;

	CClient *pClient = GetClientActive();
	if ( m_pPlayer && !pClient )	// moving a logged out client !
	{
		CSector *pSector = pt.GetSector();
		if ( !pSector )
			return false;

		// We cannot put this char in non-disconnect state.
		SetDisconnected(pSector);
        SetTopPoint(pt);
		return true;
	}

	// Did we step into a new region ?
	CRegionWorld * pAreaNew = dynamic_cast<CRegionWorld *>(pt.GetRegion(REGION_TYPE_MULTI|REGION_TYPE_AREA));
	if ( !MoveToRegion(pAreaNew, fAllowReject) )
		return false;

	CRegion * pRoomNew = pt.GetRegion(REGION_TYPE_ROOM);
	if ( !MoveToRoom(pRoomNew, fAllowReject) )
		return false;

	const CPointMap ptOld(GetTopPoint());
    SetTopPoint(pt);

	const CPointMap& ptCur = GetTopPoint();
	CSector* pNewSector = ptCur.GetSector();
	ASSERT(pNewSector);
    bool fSectorChanged = pNewSector->MoveCharToSector(this);

	if ( !m_fClimbUpdated || fForceFix )
		FixClimbHeight();

	if ( fSectorChanged && !g_Serv.IsLoading() )
	{
		if ( IsTrigUsed(TRIGGER_ENVIRONCHANGE) )
		{
			CScriptTriggerArgs Args(ptOld.m_x, ptOld.m_y, ((uchar)ptOld.m_z << 16) | ptOld.m_map);
			OnTrigger(CTRIG_EnvironChange, this, &Args);
		}
	}

    if (fCheckLocation && (CheckLocation(fStanding) == TRIGRET_RET_FALSE) && ptOld.IsValidPoint())
    {
        SetTopPoint(ptOld);
        return false;
    }
	return true;
}

bool CChar::MoveTo(const CPointMap& pt, bool fForceFix)
{
	m_fClimbUpdated = false; // update climb height
    return MoveToChar(pt, true, true, fForceFix);
}

void CChar::SetTopZ( char z )
{
	CObjBaseTemplate::SetTopZ( z );
	m_fClimbUpdated = false; // update climb height
	FixClimbHeight();	// can throw an exception
}

// Move from here to a valid spot.
// ASSUME "here" is not a valid spot. (even if it really is)
bool CChar::MoveToValidSpot(DIR_TYPE dir, int iDist, int iDistStart, bool fFromShip)
{
	ADDTOCALLSTACK("CChar::MoveToValidSpot");

	CPointMap pt = GetTopPoint();
	pt.MoveN( dir, iDistStart );
	pt.m_z += PLAYER_HEIGHT;
	char startZ = pt.m_z;

	dword dwCan = GetCanMoveFlags(GetCanFlags(), true);	// CAN_C_SWIM
	for ( int i=0; i<iDist; ++i )
	{
		if ( pt.IsValidPoint() )
		{
			// Don't allow boarding of other ships (they may be locked)
			CRegion * pRegionBase = pt.GetRegion( REGION_TYPE_SHIP);
			if ( pRegionBase)
			{
				pt.Move( dir );
				continue;
			}

			dword dwBlockFlags = dwCan;
			// Reset Z back to start Z + PLAYER_HEIGHT so we don't climb buildings
			pt.m_z = startZ;
			// Set new Z so we don't end up floating or underground
			pt.m_z = CWorldMap::GetHeightPoint( pt, dwBlockFlags, true );

			// don't allow characters to pass through walls or other blocked
			// paths when they're disembarking from a ship
			if ( fFromShip && (dwBlockFlags & CAN_I_BLOCK) && !(dwCan & CAN_C_PASSWALLS) && (pt.m_z > startZ) )
			{
				break;
			}

			if ( ! ( dwBlockFlags &~ dwCan ))
			{
				// we can go here. (maybe)
				if ( Spell_Teleport(pt, true, !fFromShip, false) )
					return true;
			}
		}
		pt.Move( dir );
	}
	return false;
}

bool CChar::MoveToNearestShore(bool fNoMsg)
{
	int iDist = 1;
	int i;
	for (i = 0; i < 20; ++i)
	{
		int iDistNew = iDist + 20;
		for (int iDir = DIR_NE; iDir <= DIR_NW; iDir += 2)	// try diagonal in all directions
		{
			if (MoveToValidSpot((DIR_TYPE)iDir, iDistNew, iDist))
			{
				i = 100;
				break;
			}
		}
		iDist = iDistNew;
	}

	if (!fNoMsg)
	{
		SysMessageDefault(i < 100 ? DEFMSG_MSG_REGION_WATER_1 : DEFMSG_MSG_REGION_WATER_2);
	}

	return (i == 100);
}

bool CChar::MoveNearObj( const CObjBaseTemplate *pObj, ushort iSteps )
{
	return CObjBase::MoveNearObj(pObj, iSteps);
}

// "PRIVSET"
// Set this char to be a GM etc. (or take this away)
// NOTE: They can be off-line at the time.
bool CChar::SetPrivLevel(CTextConsole * pSrc, lpctstr pszFlags)
{
	ADDTOCALLSTACK("CChar::SetPrivLevel");

	if ( !m_pPlayer || !pszFlags[0] || (pSrc->GetPrivLevel() < PLEVEL_Admin) || (pSrc->GetPrivLevel() < GetPrivLevel()) )
		return false;

	CAccount *pAccount = m_pPlayer->GetAccount();
	PLEVEL_TYPE PrivLevel = CAccount::GetPrivLevelText(pszFlags);

	// Remove Previous GM Robe
	ContentConsume(CResourceID(RES_ITEMDEF, ITEMID_GM_ROBE), INT32_MAX);

	if ( PrivLevel >= PLEVEL_Counsel )
	{
		pAccount->SetPrivFlags(PRIV_GM_PAGE|(PrivLevel >= PLEVEL_GM ? PRIV_GM : 0));
		StatFlag_Set(STATF_INVUL);

		UnEquipAllItems();

		CItem *pItem = CItem::CreateScript(ITEMID_GM_ROBE, this);
		if ( pItem )
		{
			pItem->SetAttr(ATTR_MOVE_NEVER|ATTR_NEWBIE|ATTR_MAGIC);
			pItem->SetHue((HUE_TYPE)((PrivLevel >= PLEVEL_GM) ? HUE_RED : HUE_BLUE_NAVY));	// since sept/2014 OSI changed 'Counselor' plevel to 'Advisor', using GM Robe color 05f
			ItemEquip(pItem);
		}
	}
	else
	{
		// Revoke GM status.
		pAccount->ClearPrivFlags(PRIV_GM_PAGE|PRIV_GM);
		StatFlag_Clear(STATF_INVUL);
	}

	pAccount->SetPrivLevel(PrivLevel);
	NotoSave_Update();
	return true;
}

bool CChar::IsTriggerActive(lpctstr trig) const
{
    if (((_iRunningTriggerId == -1) && _sRunningTrigger.empty()) || (trig == nullptr))
        return false;
    if (_iRunningTriggerId != -1)
    {
        ASSERT(_iRunningTriggerId < CTRIG_QTY);
        const int iAction = FindTableSorted( trig, CChar::sm_szTrigName, CountOf(CChar::sm_szTrigName)-1 );
        return (_iRunningTriggerId == iAction);
    }
    ASSERT(!_sRunningTrigger.empty());
	return (strcmpi(_sRunningTrigger.c_str(), trig) == 0);
}

void CChar::SetTriggerActive(lpctstr trig)
{
    if (trig == nullptr)
    {
        _iRunningTriggerId = -1;
        _sRunningTrigger.clear();
        return;
    }
	const int iAction = FindTableSorted( trig, CChar::sm_szTrigName, CountOf(CChar::sm_szTrigName)-1 );
    if (iAction != -1)
    {
        _iRunningTriggerId = (short)iAction;
        _sRunningTrigger.clear();
        return;
    }
    _sRunningTrigger = trig;
    _iRunningTriggerId = -1;
}

// Running a trigger for chars
// order:
// 1) CHAR's triggers
// 2) EVENTS
// 3) TEVENTS
// 4) CHARDEF
// 5) EVENTSPET/EVENTSPLAYER set on .ini file
// RETURNS = TRIGRET_TYPE (in cscriptobj.h)
TRIGRET_TYPE CChar::OnTrigger( lpctstr pszTrigName, CTextConsole * pSrc, CScriptTriggerArgs * pArgs )
{
	ADDTOCALLSTACK("CChar::OnTrigger");

	if ( IsTriggerActive( pszTrigName ) ) //This should protect any char trigger from infinite loop
		return TRIGRET_RET_DEFAULT;

	if ( !pSrc )
		pSrc = &g_Serv;

	CCharBase* pCharDef = Char_GetDef();
    ASSERT(pCharDef);

    SetTriggerActive( pszTrigName );
    const CTRIG_TYPE iAction = (CTRIG_TYPE)_iRunningTriggerId;
    // Attach some trigger to the cchar. (PC or NPC)
    // RETURN: true = block further action.
    TRIGRET_TYPE iRet = TRIGRET_RET_DEFAULT;

	EXC_TRY("Trigger");

	// 1) Triggers installed on characters, sensitive to actions on all chars
    {
		tchar ptcCharTrigName[TRIGGER_NAME_MAX_LEN] = "@CHAR";
		Str_ConcatLimitNull(ptcCharTrigName + 5, pszTrigName + 1, TRIGGER_NAME_MAX_LEN - 5);
        const CTRIG_TYPE iCharAction = (CTRIG_TYPE)FindTableSorted(ptcCharTrigName, sm_szTrigName, CountOf(sm_szTrigName) - 1);
        if ((iCharAction > XTRIG_UNKNOWN) && IsTrigUsed(ptcCharTrigName))
        {
            CChar* pChar = pSrc->GetChar();
            if (pChar != nullptr && this != pChar)
            {
                EXC_SET_BLOCK("chardef");
                const CUID uidOldAct = pChar->m_Act_UID;
                pChar->m_Act_UID = GetUID();
                iRet = pChar->OnTrigger(ptcCharTrigName, pSrc, pArgs);
                pChar->m_Act_UID = uidOldAct;
                if (iRet == TRIGRET_RET_TRUE)
                    goto stopandret; // Block further action.
            }
        }
    }

	//	2) EVENTS
	//
	// Go through the event blocks for the NPC/PC to do events.
	//
	if ( IsTrigUsed(pszTrigName) )
	{
		fc::vector_set<const CResourceLink*> executedEvents;

        {
            EXC_SET_BLOCK("events");
            size_t origEvents = m_OEvents.size();
            size_t curEvents = origEvents;
            for (size_t i = 0; i < curEvents; ++i) // EVENTS (could be modifyed ingame!)
            {
                CResourceLink* pLink = m_OEvents[i].GetRef();
                if (!pLink || !pLink->HasTrigger(iAction) || (executedEvents.find(pLink) != executedEvents.end()))
                    continue;

                CResourceLock s;
                if (!pLink->ResourceLock(s))
                    continue;

				executedEvents.emplace(pLink);
                iRet = CScriptObj::OnTriggerScript(s, pszTrigName, pSrc, pArgs);
                if (iRet != TRIGRET_RET_FALSE && iRet != TRIGRET_RET_DEFAULT)
                    goto stopandret;

                curEvents = m_OEvents.size();
                if (curEvents < origEvents) // the event has been deleted, modify the counter for other trigs to work
                {
                    --i;
                    origEvents = curEvents;
                }
            }
        }

		if ( m_pNPC != nullptr )
		{
			// 3) TEVENTS
			EXC_SET_BLOCK("NPC triggers"); // TEVENTS (constant events of NPCs)
			for ( size_t i = 0; i < pCharDef->m_TEvents.size(); ++i )
			{
				CResourceLink * pLink = pCharDef->m_TEvents[i].GetRef();
				if (!pLink || !pLink->HasTrigger(iAction) || (executedEvents.find(pLink) != executedEvents.end()))
					continue;

				CResourceLock s;
				if (!pLink->ResourceLock(s))
					continue;

				executedEvents.emplace(pLink);
				iRet = CScriptObj::OnTriggerScript(s, pszTrigName, pSrc, pArgs);
				if ( iRet != TRIGRET_RET_FALSE && iRet != TRIGRET_RET_DEFAULT )
					goto stopandret;
			}
		}

		// 4) CHARDEF triggers
		if ( m_pPlayer == nullptr ) //	CHARDEF triggers (based on body type)
		{
			EXC_SET_BLOCK("chardef triggers");
			if ( pCharDef->HasTrigger(iAction) )
			{
				CResourceLock s;
				if ( pCharDef->ResourceLock(s) )
				{
					iRet = CScriptObj::OnTriggerScript(s, pszTrigName, pSrc, pArgs);
					if (( iRet != TRIGRET_RET_FALSE ) && ( iRet != TRIGRET_RET_DEFAULT ))
						goto stopandret;
				}
			}
		}

		// 5) EVENTSPET triggers for npcs
		if (m_pNPC != nullptr)
		{
			EXC_SET_BLOCK("NPC triggers - EVENTSPET"); // EVENTSPET (constant events of NPCs set from sphere.ini)
			for (size_t i = 0; i < g_Cfg.m_pEventsPetLink.size(); ++i)
			{
				CResourceLink * pLink = g_Cfg.m_pEventsPetLink[i].GetRef();
				if (!pLink || !pLink->HasTrigger(iAction) || (executedEvents.find(pLink) != executedEvents.end()))
					continue;

				CResourceLock s;
				if (!pLink->ResourceLock(s))
					continue;

				executedEvents.emplace(pLink);
				iRet = CScriptObj::OnTriggerScript(s, pszTrigName, pSrc, pArgs);
				if (iRet != TRIGRET_RET_FALSE && iRet != TRIGRET_RET_DEFAULT)
					goto stopandret;
			}
		}

		// 6) EVENTSPLAYER triggers for players
		if ( m_pPlayer != nullptr )
		{
			//	EVENTSPLAYER triggers (constant events of players set from sphere.ini)
			EXC_SET_BLOCK("chardef triggers - EVENTSPLAYER");
			for ( size_t i = 0; i < g_Cfg.m_pEventsPlayerLink.size(); ++i )
			{
				CResourceLink *pLink = g_Cfg.m_pEventsPlayerLink[i].GetRef();
				if (!pLink || !pLink->HasTrigger(iAction) || (executedEvents.find(pLink) != executedEvents.end()))
					continue;

				CResourceLock s;
				if (!pLink->ResourceLock(s))
					continue;

				executedEvents.emplace(pLink);
				iRet = CScriptObj::OnTriggerScript(s, pszTrigName, pSrc, pArgs);
				if ( iRet != TRIGRET_RET_FALSE && iRet != TRIGRET_RET_DEFAULT )
					goto stopandret;
			}
		}
	}

stopandret:
    SetTriggerActive(nullptr);
    return iRet;
	EXC_CATCH;

	EXC_DEBUG_START;
	g_Log.EventDebug("trigger '%s' action '%d' [0%x]\n", pszTrigName, iAction, (dword)GetUID());
	EXC_DEBUG_END;
	return iRet;
}

TRIGRET_TYPE CChar::OnTrigger( CTRIG_TYPE trigger, CTextConsole * pSrc, CScriptTriggerArgs * pArgs )
{
	ASSERT( (trigger > CTRIG_AAAUNUSED) && (trigger < CTRIG_QTY) );
	return OnTrigger( CChar::sm_szTrigName[trigger], pSrc, pArgs );
}

// process m_fStatusUpdate flags
void CChar::OnTickStatusUpdate()
{
	ADDTOCALLSTACK("CChar::OnTickStatusUpdate");

	if ( IsClientActive() )
		GetClientActive()->UpdateStats();

	const int64 iTimeCur = CWorldGameTime::GetCurrentTime().GetTimeRaw();
	const int64 iTimeDiff = iTimeCur - _iTimeLastHitsUpdate;
	if ( g_Cfg.m_iHitsUpdateRate && ( iTimeDiff >= g_Cfg.m_iHitsUpdateRate ) )
	{
		if ( m_fStatusUpdate & SU_UPDATE_HITS )
		{
			PacketHealthUpdate *cmd = new PacketHealthUpdate(this, false);
			UpdateCanSee(cmd, m_pClient);		// send hits update to all nearby clients
			m_fStatusUpdate &= ~SU_UPDATE_HITS;
		}
		_iTimeLastHitsUpdate = iTimeCur;
	}

	if ( m_fStatusUpdate & SU_UPDATE_MODE )
	{
		UpdateMode();
		m_fStatusUpdate &= ~SU_UPDATE_MODE;
	}

	CObjBase::OnTickStatusUpdate();
}

// Food decay, decrease FOOD value.
// Call for hunger penalties if food < 40%
void CChar::OnTickFood(ushort uiVal, int HitsHungerLoss)
{
	ADDTOCALLSTACK("CChar::OnTickFood");
    if (Can(CAN_C_STATUE))
        return;
	if ( IsStatFlag(STATF_DEAD|STATF_CONJURED|STATF_SPAWNED|STATF_STONE) || !Stat_GetMaxAdjusted(STAT_FOOD) )
		return;
	if ( IsStatFlag(STATF_PET) && !NPC_CheckHirelingStatus() )		// this may be money instead of food
		return;
	if ( IsPriv(PRIV_GM) )
		return;

	// Decrease food level
	int iFood = Stat_GetVal(STAT_FOOD) - uiVal;
	if ( iFood < 0 )
		iFood = 0;
	Stat_SetVal(STAT_FOOD, (ushort)iFood);

	// Show hunger message if food level is getting low
	short iFoodLevel = Food_GetLevelPercent();
	if ( iFoodLevel > 40 )
		return;
	if ( HitsHungerLoss <= 0 || IsStatFlag(STATF_SLEEPING) )
		return;

	bool fPet = IsStatFlag(STATF_PET);
	lpctstr pszMsgLevel = Food_GetLevelMessage(fPet, false);
	SysMessagef(g_Cfg.GetDefaultMsg(DEFMSG_MSG_HUNGER), pszMsgLevel);

	char *pszMsg = Str_GetTemp();
	snprintf(pszMsg, STR_TEMPLENGTH, g_Cfg.GetDefaultMsg(DEFMSG_MSG_FOOD_LVL_LOOKS), pszMsgLevel);
	CItem *pMountItem = Horse_GetValidMountItem();
	if ( pMountItem )
		pMountItem->Emote(pszMsg);
	else
		Emote(pszMsg);

	// Get hunger damage if food level reach 0
	if ( iFoodLevel <= 0 )
	{
		OnTakeDamage(HitsHungerLoss, this, DAMAGE_FIXED);
		SoundChar(CRESND_RAND);
		if ( fPet )
			NPC_PetDesert();
	}
}

void CChar::OnTickSkill()
{
    EXC_TRYSUB("OnTickSkill");
    switch (Skill_Done())
    {
        case -SKTRIG_ABORT:
            EXC_SETSUB_BLOCK("skill abort");
            Skill_Fail(true);   // fail with no message or credit.
            break;
        case -SKTRIG_FAIL:
            EXC_SETSUB_BLOCK("skill fail");
            Skill_Fail(false);
            break;
        case -SKTRIG_QTY:
            EXC_SETSUB_BLOCK("skill cleanup");
            Skill_Cleanup();
            break;
        case -SKTRIG_STROKE:
            //EXC_SET_BLOCK("skill stroked");
            break;
    }
    EXC_CATCHSUB("Skill tick");
}

bool CChar::_CanTick() const
{
	EXC_TRY("Can tick?");

	if (IsDisconnected() && (Skill_GetActive() != NPCACT_RIDDEN))
	{
		// mounted horses can still get a tick.
		return false;
	}

	return CObjBase::_CanTick();

	EXC_CATCH;

	return false;
}

void CChar::_GoAwake()
{
	ADDTOCALLSTACK("CChar::_GoAwake");

	CObjBase::_GoAwake();
	CContainer::_GoAwake();

	CWorldTickingList::AddCharPeriodic(this, false);

	_SetTimeout(Calc_GetRandVal(1 * MSECS_PER_SEC));  // make it tick randomly in the next sector, so all awaken NPCs get a different tick time.
}

void CChar::_GoSleep()
{
	ADDTOCALLSTACK("CChar::_GoSleep");

	CContainer::_GoSleep(); // This method isn't virtual
	CObjBase::_GoSleep();

	CWorldTickingList::DelCharPeriodic(this, false);
}

// Get a timer tick when our timer expires.
// RETURN: false = delete this.
bool CChar::_OnTick()
{
    ADDTOCALLSTACK("CChar::_OnTick");

    // Assume this is only called 1 time per sec.
    // Get a timer tick when our timer expires.
    // RETURN: false = delete this.
    EXC_TRY("Tick");

	EXC_SET_BLOCK("Can Tick?");
	if ((_IsSleeping() || IsDisconnected()) && (Skill_GetActive() != NPCACT_RIDDEN))
	{
		// mounted horses can still get a tick.
		return true;
	}
    if (GetTopSector()->IsSleeping() && !Calc_GetRandVal(15))
    {
        _SetTimeout(1);      //Make it tick after sector's awakening.
        _GoSleep();
        return true;
    }

	EXC_SET_BLOCK("Components Tick");
	/*
	* CComponent's ticking:
	* Be aware that return CCRET_FALSE will return false (and delete the char),
	* take in mind that return will prevent this char's stats updates,
	*  attacker, notoriety, death status, etc from happening.
	*/
	const CCRET_TYPE iCompRet = CEntity::_OnTick();
	if (iCompRet != CCRET_CONTINUE) // if return != CCRET_TRUE
	{
		return iCompRet;    // Stop here
	}

    // My turn to do some action.
    EXC_SET_BLOCK("Timer expired");
    OnTickSkill();

    if (m_pNPC)
    {
        const ProfileTask aiTask(PROFILE_NPC_AI);
        EXC_SET_BLOCK("NPC action");
        if (!IsStatFlag(STATF_FREEZE|STATF_STONE) && !Can(CAN_C_STATUE))
        {
            NPC_OnTickAction();

            if (!IsStatFlag(STATF_DEAD))
            {
                const int iFlags = NPC_GetAiFlags();
                if ((iFlags & NPC_AI_FOOD) && !(iFlags & NPC_AI_INTFOOD))
                {
                    NPC_Food();
                }
                if (iFlags & NPC_AI_EXTRA)
                {
                    NPC_ExtraAI();
                }
            }
        }
    }

    EXC_CATCH;

//#ifdef _DEBUG
//    EXC_DEBUG_START;
//    g_Log.EventDebug("'%s' isNPC? '%d' isPlayer? '%d' client '%d' [uid=0%" PRIx16 "]\n",
//        GetName(), (int)(m_pNPC ? m_pNPC->m_Brain : 0), (int)(m_pPlayer != 0), (int)IsClientActive(), (dword)GetUID());
//    EXC_DEBUG_END;
//#endif

    return true;
}

bool CChar::OnTickPeriodic()
{
    EXC_TRY("OnTickPeriodic");
	const int64 iTimeCur = CWorldGameTime::GetCurrentTime().GetTimeRaw();

    ++_iRegenTickCount;
    _iTimeNextRegen = iTimeCur + MSECS_PER_TICK;
    const bool fRegen = (_iRegenTickCount >= TICKS_PER_SEC);

    if (fRegen)
    {
        _iRegenTickCount = 0; // Reset the regen counter

        // Periodic checks of attackers for this character.
        EXC_SET_BLOCK("last attackers");
        Attacker_CheckTimeout();    // Do this even if g_Cfg.m_iAttackerTimeout <= 0, because i may want to use the elapsed value in scripts.

        // Periodic checks of notoriety for this character.
        EXC_SET_BLOCK("NOTO timeout");
        NotoSave_CheckTimeout();    // Do this even if g_Cfg.m_iNotoTimeout <= 0, because i may want to use the elapsed value in scripts.
    }

    /*  We can only die on our own tick.
    *   Since death is not done instantly, but letting you continue the tick (preventing further checks and issues)
    *   it should also be called before stat regen, since death happen in the last tick and regen belongs to the new tick
    *   and it makes no sense to regen some hits after death.
    */
    if (!IsStatFlag(STATF_DEAD) && (Stat_GetVal(STAT_STR) <= 0))
    {
        EXC_SET_BLOCK("death");
        return Death();
    }

    // Stats regeneration
    if (!IsStatFlag(STATF_DEAD | STATF_STONE) && !Can(CAN_C_STATUE))
    {
        Stats_Regen();
    }

    // mounted horses can still die and regenerate stats, but nothing more from here.
    if (IsDisconnected())
    {
        return true;
    }

    if (IsClientActive())
    {
        CClient* pClient = GetClientActive();
        // Players have a silly "always run" flag that gets stuck on.
        if ( (iTimeCur - pClient->m_timeLastEventWalk) > (2 * MSECS_PER_TENTH) )
        {
            StatFlag_Clear(STATF_FLY);
        }

        // Check targeting timeout, if set
        if ((pClient->m_Targ_Timeout > 0) && ((iTimeCur - pClient->m_Targ_Timeout) > 0) )
        {
            pClient->addTargetCancel();
        }
    }

    if (fRegen)
    {
        // Check location periodically for standing in fire fields, traps, etc.
        EXC_SET_BLOCK("check location");
        CheckLocation(true);
    }

    EXC_SET_BLOCK("update stats");
    OnTickStatusUpdate();
    EXC_CATCH;
    return true;
}

int64 CChar::PayGold(CChar * pCharSrc, int64 iGold, CItem * pGold, ePayGold iReason)
{
    ADDTOCALLSTACK("CChar::PayGold");
    CScriptTriggerArgs Args(iGold,iReason,pGold);
    OnTrigger(CTRIG_PayGold,pCharSrc,&Args);
    return Args.m_iN1;
}
