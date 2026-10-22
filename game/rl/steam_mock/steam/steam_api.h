// steam_api.h (MOCK)
// NOT the Steamworks SDK. Only the declarations of the part of the API that game/rl/src/net/steam/*.cpp uses, so that the Steam code can be
// compiled (but not linked or run) without the SDK: CMake option MYENGINE_STEAM_CHECK builds the object library myengine_rl_steam_check against
// this file. It finds typos and wrong signatures in OUR code. Compared with the headers of the Steamworks SDK 1.65 in G6b (the real build compiled
// and ran with the same sources); when the code starts to use another call, add it here after looking it up in the real isteam*.h. Never include
// it in a build that links Steam.

#pragma once

#include <cstddef>
#include <cstdint>

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int64_t int64;
typedef uint64 SteamAPICall_t;
typedef uint32 AppId_t;

enum EResult
{
    k_EResultNone = 0,
    k_EResultOK = 1,
    k_EResultFail = 2,
    k_EResultNoConnection = 3,
};

class CSteamID
{
public:
    CSteamID() = default;
    explicit CSteamID(uint64 id)
        : id_(id)
    {
    }
    uint64 ConvertToUint64() const { return id_; }
    bool IsValid() const { return id_ != 0; }
    bool operator==(const CSteamID& other) const { return id_ == other.id_; }

private:
    uint64 id_ = 0;
};

class CGameID
{
public:
    bool IsValid() const { return id_ != 0; }
    uint32 AppID() const { return static_cast<uint32>(id_ & 0xFFFFFF); }

private:
    uint64 id_ = 0;
};

// ---- matchmaking ----------------------------------------------------------------------------------------------------------------------

enum ELobbyType
{
    k_ELobbyTypePrivate = 0,
    k_ELobbyTypeFriendsOnly = 1,
    k_ELobbyTypePublic = 2,
    k_ELobbyTypeInvisible = 3,
};

enum ELobbyComparison
{
    k_ELobbyComparisonEqual = 0,
};

enum EChatMemberStateChange
{
    k_EChatMemberStateChangeEntered = 0x0001,
    k_EChatMemberStateChangeLeft = 0x0002,
    k_EChatMemberStateChangeDisconnected = 0x0004,
    k_EChatMemberStateChangeKicked = 0x0008,
    k_EChatMemberStateChangeBanned = 0x0010,
};

enum EChatRoomEnterResponse
{
    k_EChatRoomEnterResponseSuccess = 1,
};

struct LobbyCreated_t
{
    EResult m_eResult;
    uint64 m_ulSteamIDLobby;
};
struct LobbyEnter_t
{
    uint64 m_ulSteamIDLobby;
    uint32 m_rgfChatPermissions;
    bool m_bLocked;
    uint32 m_EChatRoomEnterResponse;
};
struct LobbyMatchList_t
{
    uint32 m_nLobbiesMatching;
};
struct LobbyChatUpdate_t
{
    uint64 m_ulSteamIDLobby;
    uint64 m_ulSteamIDUserChanged;
    uint64 m_ulSteamIDMakingChange;
    uint32 m_rgfChatMemberStateChange;
};
struct LobbyDataUpdate_t
{
    uint64 m_ulSteamIDLobby;
    uint64 m_ulSteamIDMember;
    uint8 m_bSuccess;
};
struct GameLobbyJoinRequested_t
{
    CSteamID m_steamIDLobby;
    CSteamID m_steamIDFriend;
};

class ISteamMatchmaking
{
public:
    SteamAPICall_t CreateLobby(ELobbyType type, int maxMembers);
    SteamAPICall_t JoinLobby(CSteamID lobby);
    void LeaveLobby(CSteamID lobby);
    bool SetLobbyData(CSteamID lobby, const char* key, const char* value);
    const char* GetLobbyData(CSteamID lobby, const char* key);
    bool SetLobbyJoinable(CSteamID lobby, bool joinable);
    int GetNumLobbyMembers(CSteamID lobby);
    CSteamID GetLobbyMemberByIndex(CSteamID lobby, int member);
    CSteamID GetLobbyOwner(CSteamID lobby);
    void SetLobbyMemberData(CSteamID lobby, const char* key, const char* value);
    const char* GetLobbyMemberData(CSteamID lobby, CSteamID user, const char* key);
    void AddRequestLobbyListStringFilter(const char* key, const char* value, ELobbyComparison comparison);
    void AddRequestLobbyListResultCountFilter(int maxResults);
    SteamAPICall_t RequestLobbyList();
    CSteamID GetLobbyByIndex(int lobby);
    bool RequestLobbyData(CSteamID lobby);
};

enum EFriendFlags
{
    k_EFriendFlagImmediate = 0x04,
};

struct FriendGameInfo_t
{
    CGameID m_gameID;
    uint32 m_unGameIP;
    uint16 m_usGamePort;
    uint16 m_usQueryPort;
    CSteamID m_steamIDLobby;
};

class ISteamFriends
{
public:
    const char* GetPersonaName();
    const char* GetFriendPersonaName(CSteamID friendId);
    void ActivateGameOverlayInviteDialog(CSteamID lobby);
    int GetFriendCount(int friendFlags);
    CSteamID GetFriendByIndex(int friend_, int friendFlags);
    bool GetFriendGamePlayed(CSteamID friendId, FriendGameInfo_t* info);
};

class ISteamUser
{
public:
    CSteamID GetSteamID();
};

class ISteamApps
{
public:
    int GetLaunchCommandLine(char* buffer, int size);
};

// ---- networking -----------------------------------------------------------------------------------------------------------------------

struct SteamNetworkingIdentity
{
    void Clear();
    void SetSteamID64(uint64 id);
    uint64 GetSteamID64() const;
};

enum ESteamNetworkingConnectionState
{
    k_ESteamNetworkingConnectionState_None = 0,
    k_ESteamNetworkingConnectionState_Connecting = 1,
    k_ESteamNetworkingConnectionState_FindingRoute = 2,
    k_ESteamNetworkingConnectionState_Connected = 3,
    k_ESteamNetworkingConnectionState_ClosedByPeer = 4,
    k_ESteamNetworkingConnectionState_ProblemDetectedLocally = 5,
};

struct SteamNetConnectionInfo_t
{
    SteamNetworkingIdentity m_identityRemote;
    ESteamNetworkingConnectionState m_eState;
    int m_eEndReason;
};

struct SteamNetConnectionRealTimeStatus_t
{
    ESteamNetworkingConnectionState m_eState;
    int m_nPing;
};

struct SteamNetworkingMessage_t
{
    void* m_pData;
    int m_cbSize;
    SteamNetworkingIdentity m_identityPeer;
    void Release();
};

struct SteamNetworkingMessagesSessionRequest_t
{
    SteamNetworkingIdentity m_identityRemote;
};
struct SteamNetworkingMessagesSessionFailed_t
{
    SteamNetConnectionInfo_t m_info;
};

constexpr int k_nSteamNetworkingSend_Unreliable = 0;
constexpr int k_nSteamNetworkingSend_NoNagle = 1;
constexpr int k_nSteamNetworkingSend_UnreliableNoNagle = k_nSteamNetworkingSend_Unreliable | k_nSteamNetworkingSend_NoNagle;
constexpr int k_nSteamNetworkingSend_NoDelay = 4;
constexpr int k_nSteamNetworkingSend_Reliable = 8;
constexpr int k_nSteamNetworkingSend_AutoRestartBrokenSession = 32;

class ISteamNetworkingMessages
{
public:
    EResult SendMessageToUser(const SteamNetworkingIdentity& identityRemote, const void* data, uint32 size, int sendFlags, int remoteChannel);
    int ReceiveMessagesOnChannel(int localChannel, SteamNetworkingMessage_t** outMessages, int maxMessages);
    bool AcceptSessionWithUser(const SteamNetworkingIdentity& identityRemote);
    bool CloseSessionWithUser(const SteamNetworkingIdentity& identityRemote);
    ESteamNetworkingConnectionState GetSessionConnectionInfo(const SteamNetworkingIdentity& identityRemote, SteamNetConnectionInfo_t* connectionInfo,
                                                             SteamNetConnectionRealTimeStatus_t* quickStatus);
};

class ISteamNetworkingUtils
{
public:
    void InitRelayNetworkAccess();
};

// ---- the API itself and the callbacks -------------------------------------------------------------------------------------------------

bool SteamAPI_Init();
void SteamAPI_Shutdown();
void SteamAPI_RunCallbacks();
ISteamMatchmaking* SteamMatchmaking();
ISteamFriends* SteamFriends();
ISteamUser* SteamUser();
ISteamApps* SteamApps();
ISteamNetworkingMessages* SteamNetworkingMessages();
ISteamNetworkingUtils* SteamNetworkingUtils();

template <class T, class P, bool bGameServer = false>
class CCallback
{
public:
    typedef void (T::*func_t)(P*);
    CCallback(T*, func_t) {}
};

template <class T, class P>
class CCallResult
{
public:
    typedef void (T::*func_t)(P*, bool);
    void Set(SteamAPICall_t, T*, func_t) {}
    void Cancel() {}
    bool IsActive() const { return false; }
};
