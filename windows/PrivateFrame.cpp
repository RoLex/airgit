/* 
 * Copyright (C) 2001-2014 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "stdafx.h"
#include "Resource.h"

#include "PrivateFrame.h"
#include "WinUtil.h"
#include "MainFrm.h"


#include "../client/Client.h"
#include "../client/ClientManager.h"
#include "../client/Util.h"
#include "../client/LogManager.h"
#include "../client/UploadManager.h"
#include "../client/FavoriteManager.h"
#include "../client/StringTokenizer.h"
#include "../client/ResourceManager.h"
#include "../client/Adchub.h"

#include <boost/range/algorithm/for_each.hpp>

PrivateFrame::FrameMap PrivateFrame::frames;

PrivateFrame::PrivateFrame(const HintedUser& replyTo_, Client* c) : replyTo(replyTo_),
created(false), closed(false), online(replyTo_.user->isOnline()), curCommandPosition(0), failedCCPMattempts(0),
lastCCPMconnect(0),
	ctrlHubSelContainer(WC_COMBOBOXEX, this, HUB_SEL_MAP),
	ctrlMessageContainer(WC_EDIT, this, EDIT_MESSAGE_MAP),
	ctrlClientContainer(WC_EDIT, this, EDIT_MESSAGE_MAP),
	ctrlStatusContainer(STATUSCLASSNAME, this, STATUS_MSG_MAP),
	UserInfoBaseHandler(false, true)
{
	ctrlClient.setClient(c);
	ctrlClient.setPmUser(replyTo_.user);
}

LRESULT PrivateFrame::OnCreate(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled)
{
	CreateSimpleStatusBar(ATL_IDS_IDLEMESSAGE, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | SBARS_SIZEGRIP);
	ctrlStatus.Attach(m_hWndStatusBar);

	RECT r = { 0, 0, 0, 150 };
	ctrlHubSel.Create(ctrlStatus.m_hWnd, r, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN |
		WS_HSCROLL | WS_VSCROLL | CBS_DROPDOWNLIST , 0, IDC_HUB);

	ctrlHubSel.SetFont(WinUtil::systemFont);
	ctrlHubSelContainer.SubclassWindow(ctrlHubSel.m_hWnd);
	ctrlHubSel.SetImageList(ResourceLoader::getHubImages());
	
	init(m_hWnd, rcDefault);
	
	CToolInfo ti_tool(TTF_SUBCLASS, ctrlStatus.m_hWnd, STATUS_CC + POPUP_UID, 0, LPSTR_TEXTCALLBACK);
	ctrlTooltips.AddTool(&ti_tool);

	ctrlClientContainer.SubclassWindow(ctrlClient.m_hWnd);
	ctrlMessageContainer.SubclassWindow(ctrlMessage.m_hWnd);
	ctrlStatusContainer.SubclassWindow(ctrlStatus.m_hWnd);
	
	bool userBot = replyTo.user && replyTo.user->isSet(User::BOT);
	userOffline = userBot ? ResourceLoader::loadIcon(IDI_BOT_OFF) : ResourceLoader::loadIcon(IDR_PRIVATE_OFF);
	iCCReady = ResourceLoader::loadIcon(IDI_SECURE, 16);
	iStartCC = ResourceLoader::convertGrayscaleIcon(ResourceLoader::loadIcon(IDI_SECURE, 16));

	created = true;

	ClientManager::getInstance()->addListener(this);
	SettingsManager::getInstance()->addListener(this);
	ConnectionManager::getInstance()->addListener(this);

	{
		Lock l(mutex);
		conn = MainFrame::getMainFrame()->getPMConn(replyTo.user, this);
	}

	readLog();

	WinUtil::SetIcon(m_hWnd, userBot ? IDI_BOT : IDR_PRIVATE);

	//add the updateonlinestatus in the wnd message queue so the frame and tab can finish creating first.
	runSpeakerTask();

	bHandled = FALSE;
	return 1;
}
	
LRESULT PrivateFrame::onCtlColor(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& bHandled) {
	HWND hWnd = (HWND)lParam;
	HDC hDC = (HDC)wParam;
	if(hWnd == ctrlClient.m_hWnd || hWnd == ctrlMessage.m_hWnd) {
		::SetBkColor(hDC, WinUtil::bgColor);
		::SetTextColor(hDC, WinUtil::textColor);
		return (LRESULT)WinUtil::bgBrush;
	}

	bHandled = FALSE;
	return FALSE;
}

LRESULT PrivateFrame::onFocus(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	ctrlMessage.SetFocus();
	return 0;
}
	
void PrivateFrame::addClientLine(const tstring& aLine, uint8_t severity) {
	if(!created) {
		CreateEx(WinUtil::mdiClient);
	}
	setStatusText(aLine, severity);
	if (SETTING(BOLD_PM)) {
		setDirty();
	}
}

LRESULT PrivateFrame::onStatusBarClick(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled) {
	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
	CRect rect;
	ctrlStatus.GetRect(STATUS_CC, rect);
	if (PtInRect(&rect, pt)) {
		if (ccReady())
			closeCC();
		else
			startCC();
	}
	bHandled = TRUE;
	return 0;
}

LRESULT PrivateFrame::onGetToolTip(int idCtrl, LPNMHDR pnmh, BOOL& /*bHandled*/) {
	LPNMTTDISPINFO pDispInfo = (LPNMTTDISPINFO)pnmh;
	pDispInfo->szText[0] = 0;
	if (idCtrl == STATUS_CC + POPUP_UID) {
		pDispInfo->lpszText = ccReady() ? _T("Disconnect the direct encrypted channel") : _T("Start a direct encrypted channel");
	}
	return 0;
}


LRESULT PrivateFrame::OnRelayMsg(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
	LPMSG pMsg = (LPMSG)lParam;
	if(ctrlTooltips.m_hWnd != NULL && pMsg->message >= WM_MOUSEFIRST && pMsg->message <= WM_MOUSELAST)
		ctrlTooltips.RelayEvent(pMsg);
	return 0;
}

void PrivateFrame::addSpeakerTask(bool addDelay) {
	if (addDelay)
		delayEvents.addEvent(replyTo.user->getCID(), [this] { runSpeakerTask(); }, 1000);
	else
		runSpeakerTask();
}

void PrivateFrame::runSpeakerTask() {
	callAsync([this] { updateOnlineStatus(); });
}

LRESULT PrivateFrame::onHubChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& bHandled) {
	auto hp = hubs[ctrlHubSel.GetCurSel()];
	changeClient();

	updateOnlineStatus(true);

	bHandled = FALSE;
	return 0;
}

void PrivateFrame::on(ClientManagerListener::UserConnected, const OnlineUser& aUser, bool) noexcept {
	if(aUser.getUser() == replyTo.user) {
		addSpeakerTask(true); //delay this to possible show more nicks & hubs in the connect message :]
	}
}

void PrivateFrame::on(ClientManagerListener::UserDisconnected, const UserPtr& aUser, bool wentOffline) noexcept {
	if(aUser == replyTo.user) {
		if (wentOffline && ccReady())
			callAsync([this] { closeCC(true); });
		ctrlClient.setClient(nullptr);
		addSpeakerTask(wentOffline ? false : true);
	}
}

void PrivateFrame::on(ClientManagerListener::UserUpdated, const OnlineUser& aUser) noexcept {
	if (aUser.getUser() == replyTo.user) {
		callAsync([this] { updateTabIcon(false); });
	}
}
void PrivateFrame::on(ConnectionManagerListener::Connected, const ConnectionQueueItem* cqi, UserConnection* uc) noexcept{
	if (cqi->getConnType() == CONNECTION_TYPE_PM && cqi->getUser() == replyTo.user) {
		{
			Lock l(mutex);
			if (conn) {
				conn->removeListener(this);
			}
			conn = uc;
			conn->addListener(this);
		}
		lastCCPMconnect = GET_TICK();
		callAsync([this] {
			updateOnlineStatus();
			addStatusLine(_T("A direct encrypted channel has been established"), LogManager::LOG_INFO);
		});
	}
}

void PrivateFrame::on(ConnectionManagerListener::Removed, const ConnectionQueueItem* cqi) noexcept{
	if (cqi->getConnType() == CONNECTION_TYPE_PM && cqi->getUser() == replyTo.user && ccReady()) {
		{
			Lock l(mutex);
			conn = nullptr;
		}
		callAsync([this] {
			addStatusLine(_T("The direct encrypted channel has been disconnected"), LogManager::LOG_INFO);
			//If the connection lasted more than 30 seconds count it as success.
			if (lastCCPMconnect > 0 && (lastCCPMconnect + 30 * 1000) < GET_TICK()) 
				failedCCPMattempts = 0;
			else
				failedCCPMattempts++;

			updateOnlineStatus(true);
		});
	}
}

void PrivateFrame::on(ConnectionManagerListener::Failed, const ConnectionQueueItem* cqi, const string& aReason) noexcept{
	if (cqi->getConnType() == CONNECTION_TYPE_PM && cqi->getUser() == replyTo.user) {
		{
			Lock l(mutex);
			conn = nullptr;
		}
		callAsync([this, aReason] {
			addStatusLine(_T("Failed to establish direct encrypted channel: ") + Text::toT(aReason), LogManager::LOG_INFO);
			failedCCPMattempts++;
			updateOnlineStatus(true);
		});
	}
}

void PrivateFrame::on(UserConnectionListener::PrivateMessage, UserConnection* uc, const ChatMessage& message) noexcept{
	callAsync([this, message] {
		auto text = message.format();
		gotMessage(message.from->getIdentity(), message.to->getUser(), message.replyTo->getUser(), Text::toT(text), &message.from->getClient());
		MainFrame::getMainFrame()->onChatMessage(true);
	});
}


void PrivateFrame::addStatusLine(const tstring& aLine, uint8_t severity) {
	tstring status = _T(" *** ") + aLine + _T(" ***");
	if (SETTING(STATUS_IN_CHAT)) {
		addLine(status, WinUtil::m_ChatTextServer);
	}
	addClientLine(status, severity);
	
}

void PrivateFrame::changeClient() {
	replyTo.hint = hubs[ctrlHubSel.GetCurSel()].first;
	ctrlClient.setClient(ClientManager::getInstance()->getClient(replyTo.hint));
}

void PrivateFrame::updateOnlineStatus(bool ownChange) {
	const CID& cid = replyTo.user->getCID();
	const string& hint = replyTo.hint;

	dcassert(!hint.empty());

	//get the hub and online status
	auto hubsInfoNew = move(WinUtil::getHubNames(cid));
	if (!hubsInfoNew.second && !online) {
		//nothing to update... probably a delayed event or we are opening the tab for the first time
		if (nicks.empty())
			nicks = WinUtil::getNicks(HintedUser(replyTo, hint));
		if (hubNames.empty())
			hubNames = TSTRING(OFFLINE);
		

		setDisconnected(true);
		showHubSelection(false);
		updateTabIcon(true);
	} else {
		auto oldSel = ctrlHubSel.GetStyle() & WS_VISIBLE ? ctrlHubSel.GetCurSel() : 0;
		StringPair oldHubPair;
		if (!hubs.empty() && oldSel != -1)
			oldHubPair = hubs[oldSel]; // cache the old hub name

		hubs = ClientManager::getInstance()->getHubs(cid);
		while (ctrlHubSel.GetCount()) {
			ctrlHubSel.DeleteString(0);
		}

		//General things
		if (hubsInfoNew.second) {
			//the user is online

			hubNames = WinUtil::getHubNames(replyTo);
			nicks = WinUtil::getNicks(HintedUser(replyTo, hint));
			setDisconnected(false);
			updateTabIcon(false);

			if (!online) {
				addStatusLine(TSTRING(USER_WENT_ONLINE) + _T(" [") + nicks + _T(" - ") + hubNames + _T("]"), LogManager::LOG_INFO);
			}
		} else {
			if (nicks.empty())
				nicks = WinUtil::getNicks(HintedUser(replyTo, hint));

			updateTabIcon(true);
			setDisconnected(true);
			addStatusLine(TSTRING(USER_WENT_OFFLINE) + _T(" [") + hubNames + _T("]"), LogManager::LOG_INFO);
			ctrlClient.setClient(nullptr);
		}

		//ADC related changes
		if (!ccReady() && hubsInfoNew.second && !replyTo.user->isNMDC() && !hubs.empty()) {
			if (!(ctrlHubSel.GetStyle() & WS_VISIBLE)) {
				showHubSelection(true);
			}

			fillHubSelection();

			if (ownChange && ctrlHubSel.GetCurSel() != -1) {
				addStatusLine(CTSTRING_F(MESSAGES_SENT_THROUGH, Text::toT(hubs[ctrlHubSel.GetCurSel()].second)), LogManager::LOG_INFO);
			} else if (ctrlHubSel.GetCurSel() == -1) {
				//the hub was not found
				ctrlHubSel.SetCurSel(0);
				changeClient();
				if (!online) //the user came online but not in the previous hub
					addStatusLine(CTSTRING_F(MESSAGES_SENT_THROUGH, Text::toT(hubs[ctrlHubSel.GetCurSel()].second)), LogManager::LOG_WARNING);
				else
					addStatusLine(CTSTRING_F(USER_OFFLINE_PM_CHANGE, Text::toT(oldHubPair.second) % Text::toT(hubs[0].second)), LogManager::LOG_WARNING);
			} else if (!oldHubPair.first.empty() && oldHubPair.first != hint) {
				addStatusLine(CTSTRING_F(MESSAGES_SENT_THROUGH_REMOTE, Text::toT(hubs[ctrlHubSel.GetCurSel()].second)), LogManager::LOG_WARNING);
			} else if (!ctrlClient.getClient()) {
				changeClient();
			}
		}
		else if (ccReady()) {
			fillHubSelection();
			if(ctrlHubSel.GetStyle() & WS_VISIBLE)
				showHubSelection(false);
		} else {
			showHubSelection(false);
		}

		online = hubsInfoNew.second;
		checkAllwaysCCPM(); //TODO: timer task so it wont attempt again right away when failing...
	}

	SetWindowText((nicks + _T(" - ") + hubNames).c_str());
}

void PrivateFrame::checkAllwaysCCPM() {
	//StartCC will look for any hubs that we could use for CCPM, it doesn't need to be the one we use now.
	if (online && !replyTo.user->isNMDC() && SETTING(ALWAYS_CCPM) && !ccReady() && failedCCPMattempts <= 3) {
		startCC(false);
	}
}


void PrivateFrame::fillHubSelection() {
	auto* cm = ClientManager::getInstance();
	auto idents = cm->getIdentities(cm->getMe());

	for (const auto& hub : hubs) {
		auto me = idents.find(hub.first);
		int img = me == idents.end() ? 0 : me->second.isOp() ? 2 : me->second.isRegistered() ? 1 : 0;
		auto i = ctrlHubSel.AddItem(Text::toT(hub.second).c_str(), img, img, 0);
		if (hub.first == replyTo.hint) {
			ctrlHubSel.SetCurSel(i);
		}
	}
}
void PrivateFrame::showHubSelection(bool show) {
	ctrlHubSel.ShowWindow(show);
	ctrlHubSel.EnableWindow(show);

	UpdateLayout();
}

bool PrivateFrame::gotMessage(const Identity& from, const UserPtr& to, const UserPtr& replyTo, const tstring& aMessage, Client* c) {
	PrivateFrame* p = nullptr;
	bool myPM = replyTo == ClientManager::getInstance()->getMe();
	const UserPtr& user = myPM ? to : replyTo;
	
	auto hintedUser = HintedUser(user, c->getHubUrl());

	auto i = frames.find(user);
	if(i == frames.end()) {
		if(frames.size() > 200) return false;

		p = new PrivateFrame(hintedUser, c);
		frames[user] = p;
		
		p->addLine(from, aMessage);

		if(AirUtil::getAway()) {
			if(!(SETTING(NO_AWAYMSG_TO_BOTS) && user->isSet(User::BOT))) 
			{
				ParamMap params;
				from.getParams(params, "user", false);

				string error;
				p->sendMessage(Text::toT(AirUtil::getAwayMessage(p->getAwayMessage(), params)), error);
			}
		}

		if(SETTING(FLASH_WINDOW_ON_NEW_PM)) {
			WinUtil::FlashWindow();
		}

		if(SETTING(POPUP_NEW_PM)) {
			if(SETTING(PM_PREVIEW)) {
				tstring message = aMessage.substr(0, 250);
				WinUtil::showPopup(message.c_str(), CTSTRING(PRIVATE_MESSAGE));
			} else {
				WinUtil::showPopup(WinUtil::getNicks(hintedUser) + _T(" - ") + p->hubNames, TSTRING(PRIVATE_MESSAGE));
			}
		}

		if((SETTING(PRIVATE_MESSAGE_BEEP) || SETTING(PRIVATE_MESSAGE_BEEP_OPEN)) && (!SETTING(SOUNDS_DISABLED))) {
			if (SETTING(BEEPFILE).empty()) {
				MessageBeep(MB_OK);
			} else {
				WinUtil::playSound(Text::toT(SETTING(BEEPFILE)));
			}
		}
	} else {
		if(!myPM) {
			i->second->checkClientChanged(HintedUser(user, c->getHubUrl()), c, false);
			if(SETTING(FLASH_WINDOW_ON_PM) && !SETTING(FLASH_WINDOW_ON_NEW_PM)) {
				WinUtil::FlashWindow();
			}

			if(SETTING(POPUP_PM)) {
				if(SETTING(PM_PREVIEW)) {
					tstring message = aMessage.substr(0, 250);
					WinUtil::showPopup(message.c_str(), CTSTRING(PRIVATE_MESSAGE));
				} else {
					WinUtil::showPopup(WinUtil::getNicks(hintedUser) + _T(" - ") + i->second->hubNames, TSTRING(PRIVATE_MESSAGE));
				}
			}

			if((SETTING(PRIVATE_MESSAGE_BEEP)) && (!SETTING(SOUNDS_DISABLED))) {
				if (SETTING(BEEPFILE).empty()) {
					MessageBeep(MB_OK);
				} else {
					WinUtil::playSound(Text::toT(SETTING(BEEPFILE)));
				}
			}
		}
		i->second->addLine(from, aMessage);
	}
	return true;
}

void PrivateFrame::openWindow(const HintedUser& replyTo, const tstring& msg, Client* c) {
	PrivateFrame* p = nullptr;
	auto i = frames.find(replyTo);
	if(i == frames.end()) {
		if(frames.size() > 200) return;
		p = new PrivateFrame(replyTo, c);
		frames[replyTo] = p;
		p->CreateEx(WinUtil::mdiClient);
	} else {
		p = i->second;
		p->checkClientChanged(replyTo, c, true);

		if(::IsIconic(p->m_hWnd))
			::ShowWindow(p->m_hWnd, SW_RESTORE);
		p->MDIActivate(p->m_hWnd);
	}

	p->sendFrameMessage(msg);
}
/*
 update the re used frame to the correct hub, 
 so it doesnt appear offline while user is sending us messages with another hub :P
*/
void PrivateFrame::checkClientChanged(const HintedUser& newUser, Client* c, bool ownChange) {

	if(!replyTo.user->isNMDC() && replyTo.hint != newUser.hint) {
		replyTo.hint = newUser.hint;
		ctrlClient.setClient(c);
		updateOnlineStatus(ownChange);
	}
}

bool PrivateFrame::checkFrameCommand(tstring& cmd, tstring& /*param*/, tstring& /*message*/, tstring& status, bool& /*thirdPerson*/) { 
	if(stricmp(cmd.c_str(), _T("grant")) == 0) {
		UploadManager::getInstance()->reserveSlot(HintedUser(replyTo), 600);
		addClientLine(TSTRING(SLOT_GRANTED), LogManager::LOG_INFO);
	} else if(stricmp(cmd.c_str(), _T("close")) == 0) {
		PostMessage(WM_CLOSE);
	} else if((stricmp(cmd.c_str(), _T("favorite")) == 0) || (stricmp(cmd.c_str(), _T("fav")) == 0)) {
		FavoriteManager::getInstance()->addFavoriteUser(replyTo);
		addClientLine(TSTRING(FAVORITE_USER_ADDED), LogManager::LOG_INFO);
	} else if(stricmp(cmd.c_str(), _T("getlist")) == 0) {
		handleGetList();
	} else if(stricmp(cmd.c_str(), _T("log")) == 0) {
		WinUtil::openFile(Text::toT(getLogPath()));
	}
	else if (Util::stricmp(cmd.c_str(), _T("direct")) == 0 || Util::stricmp(cmd.c_str(), _T("encrypted")) == 0) {
		startCC();
	}
	else if (Util::stricmp(cmd.c_str(), _T("disconnect")) == 0) {
		closeCC();
	} else if(stricmp(cmd.c_str(), _T("help")) == 0) {
		status = _T("*** ") + ChatFrameBase::commands + _T("Additional commands for private message tabs: /getlist, /grant, /favorite");
	} else {
		return false;
	}

	return true;
}

bool PrivateFrame::sendMessage(const tstring& msg, string& error_, bool thirdPerson) {

	if (replyTo.user->isOnline()) {
		auto msg8 = Text::fromT(msg);

		{
			Lock l(mutex);
			if (conn) {
				conn->pm(msg8, thirdPerson);
				return true;
			}
		}
		return ClientManager::getInstance()->privateMessage(replyTo, msg8, error_, thirdPerson);
	}

	error_ = STRING(USER_OFFLINE);
	return false;
}

LRESULT PrivateFrame::onClose(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& bHandled) {
	if(!closed) {
		LogManager::getInstance()->removePmCache(replyTo.user);
		ClientManager::getInstance()->removeListener(this);
		SettingsManager::getInstance()->removeListener(this);
		{
			Lock l(mutex);
			if (conn) {
				conn->removeListener(this);
				conn->disconnect(true);
			}
		}

		ConnectionManager::getInstance()->removeListener(this);

		closed = true;
		PostMessage(WM_CLOSE);
		return 0;
	} else {
		auto i = frames.find(replyTo);
		if(i != frames.end())
			frames.erase(i);

		bHandled = FALSE;
		return 0;
	}
}

void PrivateFrame::startCC(bool silent) {
	if (ccReady()) {
		if (!silent) { addStatusLine(_T("A direct encrypted channel is already available"), LogManager::LOG_INFO); }
		return;
	}

	{
		tstring _err;
		RLock l(ClientManager::getInstance()->getCS());
		auto ou = ClientManager::getInstance()->getCCPMuser(replyTo, _err);
		if (!ou) {
			if (!silent) { addStatusLine(_err, LogManager::LOG_ERROR); }
			failedCCPMattempts = 3; // User does not support CCPM, no more auto connect.
			return;
		}
	}

	string _error = Util::emptyString;
	if (ConnectionManager::getInstance()->getPMConnection(replyTo.user, replyTo.hint, _error)){
		if (!silent) { addStatusLine(_T("Establishing a direct encrypted channel..."), LogManager::LOG_INFO); }
	}

	if (!_error.empty())
	{
		failedCCPMattempts++;
		addStatusLine(_T("Direct encrypted channel could not be established: ") + Text::toT(_error), LogManager::LOG_ERROR);
	}

}

void PrivateFrame::closeCC(bool silent) {
	if (ccReady()) {
		if (!silent) { addStatusLine(_T("Disconnecting the direct encrypted channel..."),LogManager::LOG_INFO); }
		ConnectionManager::getInstance()->disconnect(replyTo.user, CONNECTION_TYPE_PM);
	}
	else {
		if (!silent) { addStatusLine(_T("No direct encrypted channel available"), LogManager::LOG_INFO); }
	}
}

bool PrivateFrame::ccReady() const {
	Lock l(mutex);
	return conn;
}


void PrivateFrame::addLine(const tstring& aLine, CHARFORMAT2& cf) {
	Identity i = Identity(NULL, 0);
    addLine(i, aLine, cf);
}

void PrivateFrame::addLine(const Identity& from, const tstring& aLine) {
	addLine(from, aLine, WinUtil::m_ChatTextGeneral );
}

void PrivateFrame::fillLogParams(ParamMap& params) const {
	const CID& cid = replyTo.user->getCID();
	const string& hint = replyTo.hint;
	params["hubNI"] = [&] { return Text::fromT(hubNames); };
	params["hubURL"] = [&] { return hint; };
	params["userCID"] = [&cid] { return cid.toBase32(); };
	params["userNI"] = [&] { return ClientManager::getInstance()->getNick(replyTo.user, hint); };
	params["myCID"] = [] { return ClientManager::getInstance()->getMe()->getCID().toBase32(); };
}

void PrivateFrame::addLine(const Identity& from, const tstring& aLine, CHARFORMAT2& cf) {
	if(!created) {
		if(SETTING(POPUNDER_PM))
			WinUtil::hiddenCreateEx(this);
		else
			CreateEx(WinUtil::mdiClient);
	}

	CRect r;
	ctrlClient.GetClientRect(r);

	if(SETTING(LOG_PRIVATE_CHAT)) {
		ParamMap params;
		params["message"] = [&aLine] { return Text::fromT(aLine); };
		fillLogParams(params);
		LogManager::getInstance()->log(replyTo.user, params);
	}

	auto myNick = Text::toT(ctrlClient.getClient() ? ctrlClient.getClient()->get(HubSettings::Nick) : SETTING(NICK));
	bool notify = ctrlClient.AppendChat(from, myNick, SETTING(TIME_STAMPS) ? Text::toT("[" + Util::getShortTimeString() + "] ") : _T(""), aLine + _T('\n'), cf);
	addClientLine(TSTRING(LAST_CHANGE) + _T(" ") + Text::toT(Util::getTimeString()), LogManager::LOG_INFO);

	if(notify)
		setNotify();

	if (SETTING(BOLD_PM)) {
		setDirty();
	}
}

LRESULT PrivateFrame::onEditClearAll(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
	ctrlClient.SetWindowText(_T(""));
	return 0;
}

LRESULT PrivateFrame::onTabContextMenu(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/) {
	POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };        // location of mouse click 

	OMenu tabMenu;
	tabMenu.CreatePopupMenu();	

	tabMenu.InsertSeparatorFirst(Text::toT(ClientManager::getInstance()->getNick(replyTo.user, replyTo.hint, true)));
	if(SETTING(LOG_PRIVATE_CHAT)) {
		tabMenu.AppendMenu(MF_STRING, IDC_OPEN_USER_LOG,  CTSTRING(OPEN_USER_LOG));
		tabMenu.AppendMenu(MF_SEPARATOR);
		tabMenu.AppendMenu(MF_STRING, IDC_USER_HISTORY,  CTSTRING(VIEW_HISTORY));
		tabMenu.AppendMenu(MF_SEPARATOR);
	}
	tabMenu.AppendMenu(MF_STRING, ID_EDIT_CLEAR_ALL, CTSTRING(CLEAR_CHAT));
	appendUserItems(tabMenu, true, replyTo.user);

	prepareMenu(tabMenu, UserCommand::CONTEXT_USER, ClientManager::getInstance()->getHubUrls(replyTo.user->getCID()));
	if(!(tabMenu.GetMenuState(tabMenu.GetMenuItemCount()-1, MF_BYPOSITION) & MF_SEPARATOR)) {	
		tabMenu.AppendMenu(MF_SEPARATOR);
	}
	tabMenu.AppendMenu(MF_STRING, IDC_CLOSE_WINDOW, CTSTRING(CLOSE));

	tabMenu.open(m_hWnd, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON, pt);
	return TRUE;
}

void PrivateFrame::runUserCommand(UserCommand& uc) {

	if(!WinUtil::getUCParams(m_hWnd, uc, ucLineParams))
		return;

	auto ucParams = ucLineParams;

	ClientManager::getInstance()->userCommand(replyTo, uc, ucParams, true);
}

void PrivateFrame::UpdateLayout(BOOL bResizeBars /* = TRUE */) {
	RECT rect;
	GetClientRect(&rect);
	// position bars and offset their dimensions
	UpdateBarsPosition(rect, bResizeBars);

	if(ctrlStatus.IsWindow()) {
		
		auto setToolRect = [&] {
			CRect r;
			ctrlStatus.GetRect(STATUS_CC, r);
			ctrlTooltips.SetToolRect(ctrlStatus.m_hWnd, STATUS_CC + POPUP_UID, r);
		};

		CRect sr;
		ctrlStatus.GetClientRect(sr);

		if (ctrlHubSel.GetStyle() & WS_VISIBLE) {
			int w[STATUS_LAST];
			tstring tmp = _T(" ") + TSTRING(SEND_PM_VIA);

			int desclen = WinUtil::getTextWidth(tmp, ctrlStatus.m_hWnd);
			w[STATUS_TEXT] = sr.right - 190 - desclen - 30;
			w[STATUS_CC] = w[STATUS_TEXT] +22;
			w[STATUS_HUBSEL] = w[STATUS_CC] + desclen + 190;
			ctrlStatus.SetParts(STATUS_LAST, w);

			sr.top = 1;
			sr.left = w[STATUS_HUBSEL-1] + desclen + 10;
			sr.right = sr.left + 170;
			ctrlHubSel.MoveWindow(sr);

			ctrlStatus.SetText(STATUS_HUBSEL, tmp.c_str());
			ctrlStatus.SetIcon(STATUS_CC, iStartCC);
			setToolRect();
		}
		else if (ccReady()){
			int w[STATUS_LAST];
			tstring tmp = _T(" ") + TSTRING(SEND_PM_VIA);
			tmp += _T(": Direct encrypted channel");

			int desclen = WinUtil::getTextWidth(tmp, ctrlStatus.m_hWnd);
			w[STATUS_TEXT] = sr.right - desclen - 30;
			w[STATUS_CC] = w[STATUS_TEXT] + 22;
			w[STATUS_HUBSEL] = w[STATUS_CC] + desclen + 2;
			ctrlStatus.SetParts(STATUS_LAST, w);

			ctrlStatus.SetText(STATUS_HUBSEL, tmp.c_str());
			ctrlStatus.SetIcon(STATUS_CC, iCCReady);
			setToolRect();

		} else {
			int w[1];
			w[0] = sr.right - 16;
			ctrlStatus.SetParts(1, w);
		}
	}
	
	int h = WinUtil::fontHeight + 4;
	const int maxLines = resizePressed && SETTING(MAX_RESIZE_LINES) <= 1 ? 2 : SETTING(MAX_RESIZE_LINES);

	if((maxLines != 1) && lineCount != 0) {
		if(lineCount < maxLines) {
			h = WinUtil::fontHeight * lineCount + 4;
		} else {
			h = WinUtil::fontHeight * maxLines + 4;
		}
	} 

	CRect rc = rect;
	rc.bottom -= h + 10;
	ctrlClient.MoveWindow(rc);
	
	int buttonsize = 0;
	if(ctrlEmoticons.IsWindow() && SETTING(SHOW_EMOTICON))
		buttonsize +=26;

	if(ctrlMagnet.IsWindow())
		buttonsize += 26;

	if(ctrlResize.IsWindow())
		buttonsize += 26;

	rc = rect;
	rc.bottom -= 2;
	rc.top = rc.bottom - h - 5;
	rc.left +=2;
	rc.right -= buttonsize;
	ctrlMessage.MoveWindow(rc);

	 //ApexDC	
	if(h != (WinUtil::fontHeight + 4)) {
		rc.bottom -= h - (WinUtil::fontHeight + 4);
	}

	if(ctrlResize.IsWindow()) {
		//resize lines button
		rc.left = rc.right + 2;
		rc.right += 24;
		ctrlResize.MoveWindow(rc);
	}

	if(ctrlEmoticons.IsWindow()){
		rc.left = rc.right + 2;
  		rc.right += 24;
  		ctrlEmoticons.MoveWindow(rc);
	}
	
	if(ctrlMagnet.IsWindow()){
		//magnet button
		rc.left = rc.right + 2;
		rc.right += 24;
		ctrlMagnet.MoveWindow(rc);
	}
}

void PrivateFrame::updateTabIcon(bool offline) {
	if (offline) {
		setIcon(userOffline);
		return;
	}
	OnlineUserPtr ou = ClientManager::getInstance()->findOnlineUser(replyTo);
	if (ou) {
		tabIcon = ResourceLoader::getUserImages().GetIcon(ou->getImageIndex());
		setIcon(tabIcon);
	}
}

string PrivateFrame::getLogPath() const {
	ParamMap params;
	fillLogParams(params);
	return LogManager::getInstance()->getPath(replyTo.user, params);
}

void PrivateFrame::readLog() {
	if (SETTING(SHOW_LAST_LINES_LOG) == 0) return;
	try {
		File f(getLogPath(), File::READ, File::OPEN);
		
		int64_t size = f.getSize();

		if(size > 32*1024) {
			f.setPos(size - 32*1024);
		}
		string buf = f.read(32*1024);
		StringList lines;

		if(strnicmp(buf.c_str(), "\xef\xbb\xbf", 3) == 0)
			lines = StringTokenizer<string>(buf.substr(3), "\r\n").getTokens();
		else
			lines = StringTokenizer<string>(buf, "\r\n").getTokens();

		int linesCount = lines.size();

		int i = linesCount > (SETTING(SHOW_LAST_LINES_LOG) + 1) ? linesCount - SETTING(SHOW_LAST_LINES_LOG) : 0;

		for(; i < linesCount; ++i){
			ctrlClient.AppendChat(Identity(NULL, 0), _T("- "), _T(""), Text::toT(lines[i]) + _T('\n'), WinUtil::m_ChatTextLog, true);
		}
		f.close();
	} catch(const FileException&){
	}
}

void PrivateFrame::closeAll(){
	for(auto f: frames | map_values)
		f->PostMessage(WM_CLOSE, 0, 0);
}

void PrivateFrame::closeAllOffline() {
	for(auto& fp: frames) {
		if(!fp.first->isOnline())
			fp.second->PostMessage(WM_CLOSE, 0, 0);
	}
}

LRESULT PrivateFrame::onOpenUserLog(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {	
	string file = getLogPath();
	if(Util::fileExists(file)) {
		WinUtil::viewLog(file, wID == IDC_USER_HISTORY);
	} else {
		MessageBox(CTSTRING(NO_LOG_FOR_USER), CTSTRING(NO_LOG_FOR_USER), MB_OK );	  
	}	

	return 0;
}

void PrivateFrame::on(SettingsManagerListener::Save, SimpleXML& /*xml*/) noexcept {
	ctrlClient.SetBackgroundColor(WinUtil::bgColor);
	RedrawWindow(NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

LRESULT PrivateFrame::onPublicMessage(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {

	if(!online)
		return 0;

	tstring sUsers = ctrlClient.getSelectedUser();

	int iSelBegin, iSelEnd;
	ctrlMessage.GetSel( iSelBegin, iSelEnd );

	if ( ( iSelBegin == 0 ) && ( iSelEnd == 0 ) ) {
		sUsers += _T(": ");
		if (ctrlMessage.GetWindowTextLength() == 0) {	
			ctrlMessage.SetWindowText(sUsers.c_str());
			ctrlMessage.SetFocus();
			ctrlMessage.SetSel( ctrlMessage.GetWindowTextLength(), ctrlMessage.GetWindowTextLength() );
		} else {
			ctrlMessage.ReplaceSel( sUsers.c_str() );
			ctrlMessage.SetFocus();
		}
	} else {
		sUsers += _T(" ");
		ctrlMessage.ReplaceSel( sUsers.c_str() );
		ctrlMessage.SetFocus();
	}
	return 0;
}