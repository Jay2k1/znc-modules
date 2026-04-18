/*
 * monitor_multiclient.cpp -- a ZNC module: MONITOR multiplexer
 *
 * Virtualises the IRC MONITOR list across multiple attached clients.
 * Each client maintains its own MONITOR list (interest set); the module manages the
 * union that is actually sent to the server.  This solves two problems:
 *
 *  1. On client attach/reconnect, some IRCds such as the one used by Libera
 *     won't re-send RPL_MONONLINE / RPL_MONOFFLINE for nicks already on the 
 *     MONITOR list.  This module replays cached status toward the attaching 
 *     client directly.
 *
 *  2. One client issuing `MONITOR - nick` or `MONITOR C` no longer silently
 *     removes nicks that other attached clients still care about.
 *
 * Supported numerics (per IRCv3 MONITOR spec):
 *   730  RPL_MONONLINE
 *   731  RPL_MONOFFLINE
 *   732  RPL_MONLIST
 *   733  RPL_ENDOFMONLIST
 *   734  ERR_MONLISTFULL
 *
 * extended-monitor (AWAY state):
 *   AWAY messages from monitored nicks are intercepted and used to update
 *   the cached status, then fanned out only to clients that care.
 *
 * Installation:
 *   znc-buildmod monitor_multiclient.cpp
 *   mv monitor_multiclient.so .znc/modules/
 *   /znc loadmod monitor_multiclient       (network scope)
 *
 * This is a NETWORK module -- load it once per network, not globally!
 */

#include <znc/Modules.h>
#include <znc/Client.h>
#include <znc/IRCNetwork.h>
#include <znc/Nick.h>
#include <znc/Message.h>

#include <map>
#include <set>
#include <string>

// Case-insensitive nick comparison (IRC nicks are case-insensitive).
// ZNC's CString has CaseLess helpers; we use a simple lowercasing comparator.
struct NickLess {
    bool operator()(const CString& a, const CString& b) const {
        return a.AsLower() < b.AsLower();
    }
};

using NickSet = std::set<CString, NickLess>;

// Status we track per-nick
enum class MonitorStatus {
    Unknown,  // never seen a server reply yet
    Online,
    Offline,
    Away,     // extended-monitor: online but away
};

struct NickInfo {
    MonitorStatus status = MonitorStatus::Unknown;
    CString       nick;   // canonical capitalisation as last seen from server
    CString       host;   // nick!user@host (populated when online)
    CString       awayMsg; // populated for Away status (extended-monitor)
};

class CMonitorMod : public CModule {
  public:
	MODCONSTRUCTOR(CMonitorMod) {
		AddHelpCommand();
		AddCommand("debug", t_d("<on|off>"), t_d("Toggle debug output"),
			[this](const CString& sLine) {
				CString sArg = sLine.Token(1).AsLower();
				if (sArg == "on") {
					SetNV("debug", "1");
					PutModule("Debug output enabled.");
				} else if (sArg == "off") {
					SetNV("debug", "0");
					PutModule("Debug output disabled.");
				} else {
					PutModule("Usage: debug <on|off>  (currently: " +
							  CString(IsDebug() ? "on" : "off") + ")");
				}
			});
	}

    // -----------------------------------------------------------------------
    // Client lifecycle
    // -----------------------------------------------------------------------

    void OnClientLogin() override {
        CClient* pClient = GetClient();
        if (!pClient) return;

        // Give the new client a fresh (empty) interest set
        m_clientNicks[pClient]; // default-construct NickSet

        // Replay current cached status for all nicks we already track.
        // The client hasn't sent MONITOR + yet, so its interest set is empty --
        // we'll replay per-nick when it does send MONITOR +. Nothing to do here
        // except register the client.
    }

    void OnClientDisconnect() override {
        CClient* pClient = GetClient();
        if (!pClient) return;

        // Collect the nicks this client was interested in
        auto it = m_clientNicks.find(pClient);
        if (it == m_clientNicks.end()) return;

        NickSet clientInterest = std::move(it->second);
        m_clientNicks.erase(it);

        // For each nick this client had, check if any remaining client still
        // wants it. If not, remove from server.
        for (const CString& nick : clientInterest) {
            if (!AnyClientWants(nick)) {
                RemoveFromServer(nick);
                m_nickInfo.erase(nick.AsLower());
            }
        }
    }

    // -----------------------------------------------------------------------
    // Outgoing: intercept MONITOR commands from clients
    // -----------------------------------------------------------------------

    EModRet OnUserRaw(CString& sLine) override {
        CString sCmd = sLine.Token(0).AsUpper();
        if (sCmd != "MONITOR") return CONTINUE;

		if (IsDebug()) PutModule("OnUserRaw: Intercepted MONITOR command: " + sLine);

        CString sSubCmd = sLine.Token(1).AsUpper();

        if (sSubCmd == "+") {
            return HandleMonitorAdd(sLine.Token(2));
        } else if (sSubCmd == "-") {
            return HandleMonitorRemove(sLine.Token(2));
        } else if (sSubCmd == "C") {
            return HandleMonitorClear();
        } else if (sSubCmd == "L") {
            // Reply with this client's list, not the real server list
            return HandleMonitorList();
        } else if (sSubCmd == "S") {
            // Reply with status for this client's nicks from cache
            return HandleMonitorStatus();
        }

        return CONTINUE;
    }

    // -----------------------------------------------------------------------
    // Incoming: intercept MONITOR numerics from server
    // -----------------------------------------------------------------------

    EModRet OnNumericMessage(CNumericMessage& msg) override {
        switch (msg.GetCode()) {
            case 730: return HandleRplMonOnline(msg);
            case 731: return HandleRplMonOffline(msg);
            case 732: return HandleRplMonList(msg);   // eat server MONLIST
            case 733: return HALTCORE;                // eat end-of-MONLIST
            case 734: return HandleErrMonListFull(msg);
            default:  return CONTINUE;
        }
    }

    // Intercept AWAY from monitored nicks (extended-monitor)
    EModRet OnRawMessage(CMessage& msg) override {
        if (msg.GetCommand().AsUpper() != "AWAY") return CONTINUE;

        CString nick    = msg.GetNick().GetNick();
        CString awayMsg = msg.GetParam(0);

        CString key = nick.AsLower();
        auto infoIt = m_nickInfo.find(key);
        if (infoIt == m_nickInfo.end()) return CONTINUE; // not monitored

        if (awayMsg.empty()) {
            infoIt->second.status   = MonitorStatus::Online;
            infoIt->second.awayMsg  = "";
        } else {
            infoIt->second.status   = MonitorStatus::Away;
            infoIt->second.awayMsg  = awayMsg;
        }

        // Fan out only to clients interested in this nick
        FanOutRaw(msg.ToString(), nick);
        return HALTCORE; // we handled distribution ourselves
    }

  private:
	// read debug mode status
	bool IsDebug() const {
		return GetNV("debug") == "1";
	}

    // per-client nick interest sets
    std::map<CClient*, NickSet> m_clientNicks;

    // server-side status cache, keyed by lowercased nick
    std::map<CString, NickInfo> m_nickInfo;

    // -----------------------------------------------------------------------
    // MONITOR + handler
    // -----------------------------------------------------------------------
    EModRet HandleMonitorAdd(const CString& sNickList) {
        CClient* pClient = GetClient();
        if (!pClient) return HALTCORE;

        VCString vNicks;
        sNickList.Split(",", vNicks);

        NickSet toAddToServer;

        for (const CString& nick : vNicks) {
            if (nick.empty()) continue;

            // Register interest for this client
            m_clientNicks[pClient].insert(nick);

            CString key = nick.AsLower();
            auto infoIt = m_nickInfo.find(key);

            if (infoIt != m_nickInfo.end() &&
                infoIt->second.status != MonitorStatus::Unknown) {
                // We already have a cached status — replay it to this client
                // immediately without going to the server.
                SendStatusToClient(pClient, infoIt->second);
                if (IsDebug()) PutModule("HandleMonitorAdd: replying with already cached data for " + nick);

            } else {
                // Not yet known or first time seeing this nick — need server reply
                if (infoIt == m_nickInfo.end()) {
                    // Placeholder so we know it's being tracked
                    NickInfo info;
                    info.nick = nick;
                    m_nickInfo[key] = info;
                }
                if (IsDebug()) PutModule("HandleMonitorAdd: needing server reply for " + nick);
                toAddToServer.insert(nick);
            }
        }

        if (!toAddToServer.empty()) {
            // Forward MONITOR + only for nicks not yet tracked server-side
            CString joined;
            for (const CString& n : toAddToServer) {
                if (!joined.empty()) joined += ",";
                joined += n;
            }
            if (IsDebug()) PutModule("HandleMonitorAdd: sending to IRC: MONITOR + " + joined);

            PutIRC("MONITOR + " + joined);
        }

        return HALTCORE; // never forward the original line
    }

    // -----------------------------------------------------------------------
    // MONITOR - handler
    // -----------------------------------------------------------------------
    EModRet HandleMonitorRemove(const CString& sNickList) {
        CClient* pClient = GetClient();
        if (!pClient) return HALTCORE;

        VCString vNicks;
        sNickList.Split(",", vNicks);

        NickSet toRemoveFromServer;

        for (const CString& nick : vNicks) {
            if (nick.empty()) continue;

            // Remove from this client's interest set
            auto clientIt = m_clientNicks.find(pClient);
            if (clientIt != m_clientNicks.end()) {
                // erase is case-insensitive via NickLess comparator
                clientIt->second.erase(nick);
                if (IsDebug()) PutModule("HandleMonitorRemove: removing client interest for " + nick);
            }

            // Only remove from server if no other client still wants it
            if (!AnyClientWants(nick)) {
                toRemoveFromServer.insert(nick);
                m_nickInfo.erase(nick.AsLower());
            }
        }

        if (!toRemoveFromServer.empty()) {
            CString joined;
            for (const CString& n : toRemoveFromServer) {
                if (!joined.empty()) joined += ",";
                joined += n;
            }
            if (IsDebug()) PutModule("HandleMonitorRemove: sending to IRC: MONITOR - " + joined);
            PutIRC("MONITOR - " + joined);
        }

        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // MONITOR C handler -- clear this client's interest set only
    // -----------------------------------------------------------------------
    EModRet HandleMonitorClear() {
        CClient* pClient = GetClient();
        if (!pClient) return HALTCORE;

        auto clientIt = m_clientNicks.find(pClient);
        if (clientIt == m_clientNicks.end()) return HALTCORE;

        NickSet toRemoveFromServer;

        for (const CString& nick : clientIt->second) {
            if (!AnyClientWantsExcept(nick, pClient)) {
                toRemoveFromServer.insert(nick);
                m_nickInfo.erase(nick.AsLower());
	            if (IsDebug()) PutModule("HandleMonitorClear: clearing for client: locally removing " + nick);
            }
        }

        clientIt->second.clear();

        if (!toRemoveFromServer.empty()) {
            CString joined;
            for (const CString& n : toRemoveFromServer) {
                if (!joined.empty()) joined += ",";
                joined += n;
            }
            if (IsDebug()) PutModule("HandleMonitorClear: sending to IRC: MONITOR - " + joined);
            PutIRC("MONITOR - " + joined);
        }

        // Do NOT forward MONITOR C as that would wipe everyone's list
        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // MONITOR L -- reply with this client's list
    // -----------------------------------------------------------------------
    EModRet HandleMonitorList() {
        CClient* pClient = GetClient();
        if (!pClient) return HALTCORE;

        auto clientIt = m_clientNicks.find(pClient);
        CString myNick = GetNetwork()->GetCurNick();

        if (clientIt == m_clientNicks.end() || clientIt->second.empty()) {
            if (IsDebug()) PutModule("HandleMonitorList: sending to client: :" + GetNetwork()->GetIRCServer() + " 733 " + myNick + " :End of MONITOR list");
            pClient->PutClient(":" + GetNetwork()->GetIRCServer() + " 733 " + myNick + " :End of MONITOR list");
            return HALTCORE;
        }

        // Send 732s in batches (max ~400 chars of nicks per line, typical IRC
        // line length is 512 bytes; leave room for prefix + numeric + target)
        CString batch;
        const size_t kMaxLen = 400;

        auto flushBatch = [&]() {
            if (!batch.empty()) {
            	if (IsDebug()) PutModule("HandleMonitorList: sending to client: :" + GetNetwork()->GetIRCServer() + " 732 " + myNick + " :" + batch);
                pClient->PutClient(":" + GetNetwork()->GetIRCServer() + " 732 " + myNick + " :" + batch);
                batch.clear();
            }
        };

        for (const CString& nick : clientIt->second) {
            if (!batch.empty()) batch += ",";
            batch += nick;
            if (batch.size() >= kMaxLen) flushBatch();
        }
        flushBatch();

		if (IsDebug()) PutModule("HandleMonitorList: sending to client: :" + GetNetwork()->GetIRCServer() + " 733 " + myNick + " :End of MONITOR list");
        pClient->PutClient(":" + GetNetwork()->GetIRCServer() + " 733 " + myNick + " :End of MONITOR list");
        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // MONITOR S -- reply with cached status for this client's nicks
    // -----------------------------------------------------------------------
    EModRet HandleMonitorStatus() {
        CClient* pClient = GetClient();
        if (!pClient) return HALTCORE;

        auto clientIt = m_clientNicks.find(pClient);
        if (clientIt == m_clientNicks.end()) return HALTCORE;

        std::vector<CString> online, offline;
		NickSet unknown;

        for (const CString& nick : clientIt->second) {
            auto infoIt = m_nickInfo.find(nick.AsLower());
            if (infoIt == m_nickInfo.end() ||
                infoIt->second.status == MonitorStatus::Unknown) {
                // Genuinely unknown -- queue for a targeted MONITOR + round-trip.
                // We re-add the nick so the server will send us a fresh reply,
                // which OnNumericMessage will cache and fan out normally.
                unknown.insert(nick);
                continue;
            }
            if (infoIt->second.status == MonitorStatus::Offline) {
                offline.push_back(infoIt->second.host.empty() ? nick : infoIt->second.host);
            } else {
                online.push_back(infoIt->second.host.empty() ? nick : infoIt->second.host);
            }
        }

        // For unknown nicks: force a fresh server reply by removing and
        // re-adding them. The resulting 730/731 will update the cache and
        // be fanned out to all interested clients via OnNumericMessage.
        if (!unknown.empty()) {
            CString joined;
            for (const CString& n : unknown) {
                if (!joined.empty()) joined += ",";
                joined += n;
            }
			if (IsDebug()) PutModule("HandleMonitorStatus: sending to IRC: MONITOR - :" + joined);
            PutIRC("MONITOR - " + joined);
			if (IsDebug()) PutModule("HandleMonitorStatus: sending to IRC: MONITOR + :" + joined);
            PutIRC("MONITOR + " + joined);
        }
 
        CString myNick = GetNetwork()->GetCurNick();
        CString server = GetNetwork()->GetIRCServer();
 
        auto sendList = [&](const std::vector<CString>& list, int numeric) {
            if (list.empty()) return;
            CString batch;
            for (const CString& entry : list) {
                if (!batch.empty()) batch += ",";
                batch += entry;
                if (batch.size() >= 400) {
                    if (IsDebug()) PutModule("HandleMonitorStatus: sending to client: :" + server + " " + CString(numeric) + " " + myNick + " :" + batch);
                    pClient->PutClient(":" + server + " " + CString(numeric) + " " + myNick + " :" + batch);
                    batch.clear();
                }
            }
            if (!batch.empty()) {
				if (IsDebug()) PutModule("HandleMonitorStatus: sending to client: :" + server + " " + CString(numeric) + " " + myNick + " :" + batch);
                pClient->PutClient(":" + server + " " + CString(numeric) + " " + myNick + " :" + batch);
            }
        };
 
		if (IsDebug()) PutModule("HandleMonitorStatus: replying with cached MONITOR status for client");
        sendList(online,  730);
        sendList(offline, 731);
 
        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // Incoming 730 RPL_MONONLINE -- update cache, fan out to interested clients
    // -----------------------------------------------------------------------
    EModRet HandleRplMonOnline(CNumericMessage& msg) {
        // Format: :server 730 yournick :nick!user@host[,nick!user@host,...]
        CString sParam = msg.GetParam(1);
        sParam.TrimPrefix(":");

        VCString vEntries;
        sParam.Split(",", vEntries);

        for (const CString& entry : vEntries) {
            CString nick = entry.Token(0, false, "!");
            CString key  = nick.AsLower();

            NickInfo& info = m_nickInfo[key];
            info.nick   = nick;
            info.host   = entry;
            info.status = MonitorStatus::Online;
        }

		if (IsDebug()) PutModule("HandleRplMonOnline: received 730, sending out to interested clients");
        // Fan out to each client that cares about at least one of these nicks
        FanOutNumeric(730, sParam, vEntries);
        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // Incoming 731 RPL_MONOFFLINE
    // -----------------------------------------------------------------------
    EModRet HandleRplMonOffline(CNumericMessage& msg) {
        CString sParam = msg.GetParam(1);
        sParam.TrimPrefix(":");

        VCString vEntries;
        sParam.Split(",", vEntries);

        for (const CString& entry : vEntries) {
            // Offline entries may just be the nick (no host)
            CString nick = entry.Token(0, false, "!");
            CString key  = nick.AsLower();

            NickInfo& info = m_nickInfo[key];
            info.nick   = nick;
            info.status = MonitorStatus::Offline;
            // host stays as it was (or empty)
        }

		if (IsDebug()) PutModule("HandleRplMonOffine: received 731, sending out to interested clients");
        FanOutNumeric(731, sParam, vEntries);
        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // Incoming 732 RPL_MONLIST -- eat it; we answer MONITOR L ourselves
    // -----------------------------------------------------------------------
    EModRet HandleRplMonList(CNumericMessage&) {
        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // Incoming 734 ERR_MONLISTFULL -- forward to the client that caused it
    // -----------------------------------------------------------------------
    EModRet HandleErrMonListFull(CNumericMessage& msg) {
        // Forward to the currently active client only (the one whose MONITOR +
        // triggered this). GetClient() returns the right one inside a hook.
        CClient* pClient = GetClient();
        if (pClient) {
            pClient->PutClient(msg.ToString());
        }
        return HALTCORE;
    }

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    // Returns true if any client (optionally excluding pExclude) is interested
    // in this nick.
    bool AnyClientWants(const CString& nick, CClient* pExclude = nullptr) {
        for (auto& [pClient, nicks] : m_clientNicks) {
            if (pClient == pExclude) continue;
            if (nicks.count(nick)) return true;
        }
        return false;
    }

    bool AnyClientWantsExcept(const CString& nick, CClient* pExclude) {
        return AnyClientWants(nick, pExclude);
    }

    void RemoveFromServer(const CString& nick) {
        if (IsDebug()) PutModule("RemoveFromServer: sending to IRC: MONITOR - " + nick);
        PutIRC("MONITOR - " + nick);
    }

    // Send a synthetic 730/731 line to a specific client for one NickInfo
    void SendStatusToClient(CClient* pClient, const NickInfo& info) {
        CString myNick = GetNetwork()->GetCurNick();
        CString server = GetNetwork()->GetIRCServer();

        int numeric = (info.status == MonitorStatus::Offline) ? 731 : 730;
        CString entry = info.host.empty() ? info.nick : info.host;

        if (IsDebug()) PutModule("SendStatusToClient: sending to client: :" + server + " " + CString(numeric) + " " + myNick + " :" + entry);
        pClient->PutClient(":" + server + " " + CString(numeric) + " " + myNick + " :" + entry);
    }

    // Fan out a MONITOR 730/731 numeric to every client interested in at least
    // one of the nicks in vEntries, sending only the nicks they care about.
    void FanOutNumeric(int numeric,
                       const CString& /*fullParam*/,
                       const VCString& vEntries) {
        CString myNick = GetNetwork()->GetCurNick();
        CString server = GetNetwork()->GetIRCServer();

        for (auto& [pClient, clientNicks] : m_clientNicks) {
            // Collect entries this client cares about
            CString batch;
            for (const CString& entry : vEntries) {
                CString nick = entry.Token(0, false, "!");
                if (clientNicks.count(nick)) {
                    if (!batch.empty()) batch += ",";
                    batch += entry;
                }
            }
            if (!batch.empty()) {
	            if (IsDebug()) PutModule("FanOutNumeric: sending to client: :" + server + " " + CString(numeric) + " " + myNick + " :" + batch);
                pClient->PutClient(":" + server + " " + CString(numeric) + " " + myNick + " :" + batch);
            }
        }
    }

    // Fan out a raw line (e.g. AWAY) to every client interested in `nick`
    void FanOutRaw(const CString& sLine, const CString& nick) {
        for (auto& [pClient, clientNicks] : m_clientNicks) {
            if (clientNicks.count(nick)) {
	            if (IsDebug()) PutModule("FanOutRaw: sending to client: " + sLine);
                pClient->PutClient(sLine);
            }
        }
    }
};

NETWORKMODULEDEFS(CMonitorMod,
    "Virtualises the IRC MONITOR list across multiple attached clients. "
    "Prevents clients from clobbering each other's MONITOR state and "
    "replays cached online/offline status to newly attached clients.")
