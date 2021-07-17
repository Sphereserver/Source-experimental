/**
* @file CItemMultiCustom.h
*
*/

#ifndef _INC_CITEMMULTICUSTOM_H
#define _INC_CITEMMULTICUSTOM_H

#include "CItemMulti.h"
#include "../components/CCMultiMovable.h"


class PacketHouseDesign;

class CItemMultiCustom : public CItemMulti
{
// IT_MULTI_CUSTOM
// A customizable multi
public:
    struct CMultiComponent
    {
        CUOMultiItemRec_HS m_item;
        short m_isStair;
        bool m_isFloor;
    };

private:
    struct CDesignDetails
    {
        int m_iRevision;
        std::vector<CMultiComponent*> m_vectorComponents;
        PacketHouseDesign* m_pData;
        int m_iDataRevision;
    };

    class CSphereMultiCustom : public CUOMulti
    {
    public:
        CSphereMultiCustom() = default;
        void LoadFrom(CDesignDetails * pDesign);

    private:
        CSphereMultiCustom(const CSphereMultiCustom& copy);
        CSphereMultiCustom& operator=(const CSphereMultiCustom& other);
    };

private:
    static lpctstr const sm_szLoadKeys[];
    static lpctstr const sm_szVerbKeys[];

    CDesignDetails m_designMain;
    CDesignDetails m_designWorking;
    CDesignDetails m_designBackup;
    CDesignDetails m_designRevert;

    CClient * m_pArchitect;
    CRectMap m_rectDesignArea;
    CSphereMultiCustom * m_pSphereMulti;
    int _iMaxPlane;

    virtual bool r_GetRef(lpctstr & ptcKey, CScriptObj * & pRef) override;
    virtual void r_Write(CScript & s) override;
    virtual bool r_WriteVal(lpctstr ptcKey, CSString & sVal, CTextConsole * pSrc = nullptr, bool fNoCallParent = false, bool fNoCallChildren = false) override;
    virtual bool r_LoadVal(CScript & s) override;
    virtual bool r_Verb(CScript & s, CTextConsole * pSrc) override; // Execute command from script

    CPointMap GetComponentPoint(const CMultiComponent * pComponent) const;
    CPointMap GetComponentPoint(short dx, short dy, char dz) const;
    
    /**
    * @brief Removes a CMultiComponent from the components list.
    * @param pComponent the component.
    */
    virtual void DeleteComponent(const CUID& uidComponent, bool fRemoveFromList) override final;

    void CopyDesign(CDesignDetails * designFrom, CDesignDetails * designTo);
    void GetLockdownsAt(short dx, short dy, char dz, std::vector<CUID> &vList);
    void GetSecuredAt(short dx, short dy, char dz, std::vector<CUID> &vList);
    char CalculateLevel(char z);
    void ClearFloor(char iFloor);

private:
    using ValidItemsContainer = std::map<ITEMID_TYPE, uint>;	// ItemID, FeatureMask
    static ValidItemsContainer sm_mapValidItems;

    static bool LoadValidItems();

public:
    static const char *m_sClassName;
    CItemMultiCustom(ITEMID_TYPE id, CItemBase * pItemDef);
    virtual ~CItemMultiCustom();

private:
    CItemMultiCustom(const CItemMultiCustom& copy);
    CItemMultiCustom& operator=(const CItemMultiCustom& other);

public:
    void BeginCustomize(CClient* pClientSrc, bool continueCustomize = false);
    void EndCustomize(bool fForce = false);
    void SwitchToLevel(CClient * pClientSrc, uchar iLevel);
    void CommitChanges(CClient * pClientSrc = nullptr);
    void AddItem(CClient * pClientSrc, ITEMID_TYPE id, short x, short y, char z = INT8_MIN, short iStairID = 0);
    void AddStairs(CClient * pClientSrc, ITEMID_TYPE id, short x, short y, char z = INT8_MIN);
    void AddRoof(CClient * pClientSrc, ITEMID_TYPE id, short x, short y, char z);
    void RemoveItem(CClient * pClientSrc, ITEMID_TYPE id, short x, short y, char z);
    bool RemoveStairs(CMultiComponent * pStairComponent);
    void RemoveRoof(CClient * pClientSrc, ITEMID_TYPE id, short x, short y, char z);
    void SendVersionTo(CClient * pClientSrc) const;
    void SendStructureTo(CClient * pClientSrc);
    void BackupStructure();
    void RestoreStructure(CClient * pClientSrc = nullptr);
    void RevertChanges(CClient * pClientSrc = nullptr);
    void ResetStructure(CClient * pClientSrc = nullptr);

    const CSphereMultiCustom * GetMultiItemDefs();
    const CRect GetDesignArea();
    size_t GetFixtureCount(CDesignDetails * pDesign = nullptr);
    size_t GetComponentsAt(short dx, short dy, char dz, CMultiComponent ** pComponents, CDesignDetails * pDesign = nullptr);
    int GetRevision(const CClient * pClientSrc = nullptr) const;
    uchar GetLevelCount();

    static uchar GetPlane(char z);
    static uchar GetPlane(const CMultiComponent * pComponent);
    static char GetPlaneZ(uchar plane);
    static bool IsValidItem(ITEMID_TYPE id, CClient * pClientSrc, bool fMulti);

    CDesignDetails* GetDesignMain()
    {
        return &m_designMain;
    }
};


#endif // _INC_CITEMMULTICUSTOM_H
