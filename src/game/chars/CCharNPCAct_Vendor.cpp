// Actions specific to an NPC.

#include "../../network/receive.h"
#include "../clients/CClient.h"
#include "../CWorld.h"
#include "../CPathFinder.h"
#include "../spheresvr.h"
#include "../triggers.h"
#include "CChar.h"
#include "CCharNPC.h"

//////////////////////////
// CChar

const LAYER_TYPE CChar::sm_VendorLayers[] = // static
{
	LAYER_VENDOR_STOCK, LAYER_VENDOR_EXTRA, LAYER_VENDOR_BUYS,
};

bool CChar::NPC_IsVendor() const
{
	return (m_pNPC && m_pNPC->IsVendor());
}

int CChar::NPC_GetAiFlags()
{
	ASSERT(m_pNPC);
	return (m_pNPC->GetNpcAiFlags(this));
}

bool CChar::NPC_Vendor_Restock(bool bForce, bool bFillStock)
{
	ADDTOCALLSTACK("CChar::NPC_Vendor_Restock");
	ASSERT(m_pNPC);
	// Restock this NPC char.
	// Then Set the next restock time for this .

	// Make sure that we're a vendor and not a pet

	if ( IsStatFlag(STATF_PET) || !NPC_IsVendor() )
		return false;

	bool bRestockNow = false;
    int64 iRestockDelay = 10 * 60 * MSECS_PER_SEC;  // 10 Minutes delay

	if ( !bForce && (g_World.GetTimeDiff(m_pNPC->m_timeRestock) < 0))
	{
        bRestockNow = true; // restock timeout has expired, make it restock again (unless it's declared to do not restock in the bellow lines).
		CRegionWorld *region = GetRegion();
		if( region != nullptr )
		{
			CVarDefCont *vardef = region->m_TagDefs.GetKey("RestockVendors");
			if( vardef != nullptr )
                iRestockDelay = vardef->GetValNum() * MSECS_PER_TENTH;  // backwards: it was working on tenths in scripts before, keep it like that and update it to seconds.
			if ( region->m_TagDefs.GetKey("NoRestock") != nullptr )
				bRestockNow = false;
		}
		if ( m_TagDefs.GetKey("NoRestock") != nullptr )
			bRestockNow = false;
	}
    int64 iNextRestock = g_World.GetCurrentTime().GetTimeRaw() + iRestockDelay;
    
	// At restock the containers are actually emptied
	if ( bRestockNow )
	{
		for ( size_t i = 0; i < CountOf(sm_VendorLayers); ++i )
		{
			CItemContainer *pCont = GetBank(sm_VendorLayers[i]);
			if ( !pCont )
				return false;

			pCont->Clear();
		}
        bFillStock = true;  // force the vendor to restock.
	}

	if ( bFillStock )
	{
		// An invalid restock time means that the containers are
		// waiting to be filled
		if ( IsTrigUsed(TRIGGER_NPCRESTOCK) )
		{
			CCharBase *pCharDef = Char_GetDef();
			ReadScriptTrig(pCharDef, CTRIG_NPCRestock, true);
		}

		//	we need restock vendor money as well
		GetBank()->Restock();

		// remember that the stock was filled (or considered up-to-date)
        m_pNPC->m_timeRestock = iNextRestock;
	}
	return true;
}

bool CChar::NPC_StablePetSelect( CChar * pCharPlayer )
{
	ADDTOCALLSTACK("CChar::NPC_StablePetSelect");
	ASSERT(m_pNPC);
	// I am a stable master.
	// I will stable a pet for the player.

	if ( pCharPlayer == nullptr )
		return false;
	if ( ! pCharPlayer->IsClient())
		return false;

	// Might have too many pets already ?
	int iCount = 0;
	CItemContainer * pBank = GetBank();
	if ( pBank->GetCount() >= g_Cfg.m_iContainerMaxItems )
	{
		Speak( g_Cfg.GetDefaultMsg( DEFMSG_NPC_STABLEMASTER_FULL ) );
		return false;
	}

	// Calculate the max limit of pets that the NPC can hold for the player
	double iSkillTaming = pCharPlayer->Skill_GetAdjusted(SKILL_TAMING);
	double iSkillAnimalLore = pCharPlayer->Skill_GetAdjusted(SKILL_ANIMALLORE);
	double iSkillVeterinary = pCharPlayer->Skill_GetAdjusted(SKILL_VETERINARY);
	double iSkillSum = iSkillTaming + iSkillAnimalLore + iSkillVeterinary;

	int iPetMax;
	if ( iSkillSum >= 240.0 )
		iPetMax = 5;
	else if ( iSkillSum >= 200.0 )
		iPetMax = 4;
	else if ( iSkillSum >= 160.0 )
		iPetMax = 3;
	else
		iPetMax = 2;

	if ( iSkillTaming >= 100.0 )
		iPetMax += (int)((iSkillTaming - 90.0) / 10);

	if ( iSkillAnimalLore >= 100.0 )
		iPetMax += (int)((iSkillAnimalLore - 90.0) / 10);

	if ( iSkillVeterinary >= 100.0 )
		iPetMax += (int)((iSkillVeterinary - 90.0) / 10);

	int iMaxPlayerPets = (int)m_TagDefs.GetKeyNum("MAXPLAYERPETS");
	if ( iMaxPlayerPets )
		iPetMax = iMaxPlayerPets;

	for ( CItem *pItem = pBank->GetContentHead(); pItem != nullptr; pItem = pItem->GetNext() )
	{
		if ( pItem->IsType(IT_FIGURINE) && pItem->m_uidLink == pCharPlayer->GetUID() )
			++iCount;
	}
	if ( iCount >= iPetMax )
	{
		Speak( g_Cfg.GetDefaultMsg( DEFMSG_NPC_STABLEMASTER_TOOMANY ) );
		return false;
	}

	pCharPlayer->m_pClient->m_Targ_Prv_UID = GetUID();
	pCharPlayer->m_pClient->addTarget( CLIMODE_TARG_PET_STABLE, g_Cfg.GetDefaultMsg( DEFMSG_NPC_STABLEMASTER_TARG ) );
	return true;
}

bool CChar::NPC_StablePetRetrieve( CChar * pCharPlayer )
{
	ADDTOCALLSTACK("CChar::NPC_StablePetRetrieve");
	ASSERT(m_pNPC);
	// Get pets for this person from my inventory.
	// May want to put up a menu ???

	if ( m_pNPC->m_Brain != NPCBRAIN_STABLE )
		return false;

	int iCount = 0;
	CItem *pItemNext = nullptr;
	for ( CItem *pItem = GetBank()->GetContentHead(); pItem != nullptr; pItem = pItemNext )
	{
		pItemNext = pItem->GetNext();
		if ( pItem->IsType(IT_FIGURINE) && pItem->m_uidLink == pCharPlayer->GetUID() )
		{
			if ( !pCharPlayer->Use_Figurine(pItem) )
			{
				tchar *pszTemp = Str_GetTemp();
				sprintf(pszTemp, g_Cfg.GetDefaultMsg(DEFMSG_NPC_STABLEMASTER_CLAIM_FOLLOWER), pItem->GetName());
				Speak(pszTemp);
				return true;
			}

			pItem->Delete();
			++iCount;
		}
	}

	Speak(g_Cfg.GetDefaultMsg((iCount > 0) ? DEFMSG_NPC_STABLEMASTER_CLAIM : DEFMSG_NPC_STABLEMASTER_CLAIM_NOPETS));
	return true;
}

ushort CChar::NPC_OnTrainCheck( CChar * pCharSrc, SKILL_TYPE Skill )
{
	ADDTOCALLSTACK("CChar::NPC_OnTrainCheck");
	ASSERT(m_pNPC);
	// Can we train in this skill ?
	// RETURN: Amount of skill we can train.

	if ( !IsSkillBase(Skill) )
	{
		Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_TRAINER_DUNNO_1));
		return 0;
	}

	ushort uiSkillSrcVal = pCharSrc->Skill_GetBase(Skill);
	ushort uiSkillVal = Skill_GetBase(Skill);
	ushort uiTrainVal = NPC_GetTrainMax(pCharSrc, Skill) - uiSkillSrcVal;

	// Train npc skill cap
	ushort uiMaxDecrease = 0;
	if ( (pCharSrc->GetSkillTotal() + uiTrainVal) > pCharSrc->Skill_GetMax((SKILL_TYPE)(g_Cfg.m_iMaxSkill)) )
	{	
		for ( size_t i = 0; i < g_Cfg.m_iMaxSkill; ++i )
		{
			if ( !g_Cfg.m_SkillIndexDefs.IsValidIndex((SKILL_TYPE)i) )
				continue;

			if ( pCharSrc->Skill_GetLock((SKILL_TYPE)i) == SKILLLOCK_DOWN )
				uiMaxDecrease += pCharSrc->Skill_GetBase((SKILL_TYPE)i);
		}
		uiMaxDecrease = minimum(uiTrainVal, uiMaxDecrease);
	}
	else
	{
		uiMaxDecrease = uiTrainVal;
	}

	lpctstr pszMsg;
	if ( uiSkillVal <= 0 )
		pszMsg = g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_DUNNO_2 );
	else if ( uiSkillSrcVal > uiSkillVal )
		pszMsg = g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_DUNNO_3 );
	else if ( uiMaxDecrease <= 0 )
		pszMsg = g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_DUNNO_4 );
	else
		return uiMaxDecrease;

	char *z = Str_GetTemp();
	sprintf(z, pszMsg, g_Cfg.GetSkillKey(Skill));
	Speak(z);
	return 0;
}

bool CChar::NPC_OnTrainPay(CChar *pCharSrc, CItemMemory *pMemory, CItem * pGold)
{
	ADDTOCALLSTACK("CChar::NPC_OnTrainPay");
	ASSERT(m_pNPC);

	SKILL_TYPE skill = (SKILL_TYPE)(pMemory->m_itEqMemory.m_Skill);
	if ( !IsSkillBase(skill) || !g_Cfg.m_SkillIndexDefs.IsValidIndex(skill) )
	{
		Speak(g_Cfg.GetDefaultMsg(DEFMSG_NPC_TRAINER_FORGOT));
		return false;
	}

    ushort uiTrainVal = NPC_OnTrainCheck(pCharSrc, skill);
    ushort uiTrainMult = (ushort)GetKeyNum("OVERRIDE.TRAINSKILLCOST");
	if ( !uiTrainMult)
        uiTrainMult = (ushort)g_Cfg.m_iTrainSkillCost;

    word wTrainCost = (word)pCharSrc->PayGold(this, (word)minimum(UINT16_MAX, uiTrainVal * uiTrainMult), pGold, PAYGOLD_TRAIN);

	if ( (wTrainCost <= 0) || !pGold )
		return false;

	Speak( g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_SUCCESS ) );

	// Can't ask for more gold than the maximum amount of the gold stack i am giving to the npc

	// Consume as much money as we can train for.
	if ( pGold->GetAmount() < wTrainCost )
	{
		int iDiffPercent = IMulDiv(wTrainCost, 100, pGold->GetAmount());
		uiTrainVal = (ushort)IMulDiv(uiTrainVal,100,iDiffPercent);
        wTrainCost = (word)pCharSrc->PayGold(this, (word)minimum(UINT16_MAX, uiTrainVal * uiTrainMult), pGold, PAYGOLD_TRAIN);
	}
	else if ( pGold->GetAmount() == wTrainCost)
	{
		Speak( g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_THATSALL_1 ) );
		pMemory->m_itEqMemory.m_Action = NPC_MEM_ACT_NONE;
	}
	else
	{
		Speak( g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_THATSALL_2 ) );
		pMemory->m_itEqMemory.m_Action = NPC_MEM_ACT_NONE;

		// Give change back.
		pGold->UnStackSplit( wTrainCost, pCharSrc );
	}
	GetPackSafe()->ContentAdd( pGold );	// take my cash.

	// Give credit for training.
	NPC_TrainSkill( pCharSrc, skill, uiTrainVal);
	return true;
}

bool CChar::NPC_TrainSkill( CChar * pCharSrc, SKILL_TYPE skill, ushort uiAmountToTrain )
{
	ADDTOCALLSTACK("CChar::NPC_TrainSkill");
	ASSERT(m_pNPC);

	ushort iTrain = uiAmountToTrain;
	if ( (pCharSrc->GetSkillTotal() + uiAmountToTrain) > pCharSrc->Skill_GetMax((SKILL_TYPE)(g_Cfg.m_iMaxSkill)) )
	{	
		for ( uint i = 0; i < g_Cfg.m_iMaxSkill; ++i )
		{
			if ( !g_Cfg.m_SkillIndexDefs.IsValidIndex((SKILL_TYPE)i) )
				continue;

			if ( uiAmountToTrain < 1 )
			{
				pCharSrc->Skill_SetBase(skill, iTrain + pCharSrc->Skill_GetBase(skill));
				break;
			}

			if ( pCharSrc->Skill_GetLock((SKILL_TYPE)i) == SKILLLOCK_DOWN )
			{
				if ( pCharSrc->Skill_GetBase((SKILL_TYPE)i) > uiAmountToTrain )
				{
					pCharSrc->Skill_SetBase((SKILL_TYPE)i, pCharSrc->Skill_GetBase((SKILL_TYPE)i) - uiAmountToTrain);
					uiAmountToTrain = 0;
				}
				else
				{
					uiAmountToTrain -= pCharSrc->Skill_GetBase((SKILL_TYPE)i);
					pCharSrc->Skill_SetBase((SKILL_TYPE)i, 0);
				}
			}
		}
	}
	else
	{
		pCharSrc->Skill_SetBase(skill, iTrain + pCharSrc->Skill_GetBase(skill));
	}

	return true;
}

bool CChar::NPC_OnTrainHear( CChar * pCharSrc, lpctstr pszCmd )
{
	ADDTOCALLSTACK("CChar::NPC_OnTrainHear");
	ASSERT(m_pNPC);
	// We are asking for training ?

	// Check the NPC is capable of teaching
	if ( (m_pNPC->m_Brain < NPCBRAIN_HUMAN) || (m_pNPC->m_Brain > NPCBRAIN_STABLE) || (m_pNPC->m_Brain == NPCBRAIN_GUARD) )
		return false;

	// Check the NPC isn't busy fighting
	if ( Memory_FindObjTypes( pCharSrc, MEMORY_FIGHT|MEMORY_HARMEDBY|MEMORY_IRRITATEDBY|MEMORY_AGGREIVED ))
	{
		Speak( g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_ENEMY ) );
		return true;
	}

	// Did they mention a skill name i recognize ?
	TemporaryString tsMsg;
	tchar* pszMsg = static_cast<tchar *>(tsMsg);

	for ( size_t i = 0; i < g_Cfg.m_iMaxSkill; ++i )
	{
		if ( !g_Cfg.m_SkillIndexDefs.IsValidIndex((SKILL_TYPE)i) )
			continue;

		lpctstr pSkillKey = g_Cfg.GetSkillKey((SKILL_TYPE)i);
		if ( FindStrWord( pszCmd, pSkillKey ) <= 0)
			continue;

		// Can we train in this ?
		int iTrainCost = NPC_OnTrainCheck(pCharSrc, (SKILL_TYPE)i) * g_Cfg.m_iTrainSkillCost;
		if ( iTrainCost <= 0 )
			return true;

		sprintf(pszMsg, g_Cfg.GetDefaultMsg(DEFMSG_NPC_TRAINER_PRICE), iTrainCost, pSkillKey);
		Speak(pszMsg);
		CItemMemory * pMemory = Memory_AddObjTypes( pCharSrc, MEMORY_SPEAK );
		if ( pMemory )
		{
			pMemory->m_itEqMemory.m_Action = NPC_MEM_ACT_SPEAK_TRAIN;
			pMemory->m_itEqMemory.m_Skill = (word)(i);
		}
		return true;
	}

	// What can he teach me about ?
	// Just tell them what we can teach them or set up a memory to train.
	strcpy( pszMsg, g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_PRICE_1 ) );

	lpctstr pPrvSkill = nullptr;

	size_t iCount = 0;
	for ( size_t i = 0; i < g_Cfg.m_iMaxSkill; i++ )
	{
		if ( !g_Cfg.m_SkillIndexDefs.IsValidIndex((SKILL_TYPE)i) )
			continue;

		int iDiff = NPC_GetTrainMax(pCharSrc, (SKILL_TYPE)i) - pCharSrc->Skill_GetBase((SKILL_TYPE)i);
		if ( iDiff <= 0 )
			continue;

		if ( iCount > 6 )
		{
			pPrvSkill = g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_PRICE_2 );
			break;
		}
		if ( iCount > 1 )
		{
			strcat( pszMsg, g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_PRICE_3 ) );
		}
		if ( pPrvSkill )
		{
			strcat( pszMsg, pPrvSkill );
		}

		pPrvSkill = g_Cfg.GetSkillKey((SKILL_TYPE)i);
		iCount++;
	}

	if ( iCount == 0 )
	{
		Speak( g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_THATSALL_3 ) );
		return true;
	}
	if ( iCount > 1 )
	{
		strcat( pszMsg, g_Cfg.GetDefaultMsg( DEFMSG_NPC_TRAINER_THATSALL_4 ) );
	}

	strcat( pszMsg, pPrvSkill );
	strcat( pszMsg, "." );
	Speak( pszMsg );
	return true;
}
