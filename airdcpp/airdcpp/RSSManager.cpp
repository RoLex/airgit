
#include "stdinc.h"
#include "DCPlusPlus.h"

#include "HttpConnection.h"
#include "RSSManager.h"
#include "LogManager.h"
#include "SearchManager.h"
#include "ScopedFunctor.h"
#include "AirUtil.h"
#include <boost/algorithm/string/trim.hpp>

namespace dcpp {

#define CONFIG_NAME "RSS.xml"
#define CONFIG_DIR Util::PATH_USER_CONFIG

RSSManager::RSSManager() { }

RSSManager::~RSSManager()
{
	TimerManager::getInstance()->removeListener(this);
}

void RSSManager::clearRSSData(const RSSPtr& aFeed) {
	
	{
		Lock l(cs);
		aFeed->getFeedData().clear(); 
	}
	fire(RSSManagerListener::RSSDataCleared(), aFeed);

}

RSSPtr RSSManager::getFeedByCategory(const string& aCategory) {
	Lock l(cs);
	auto r = find_if(rssList.begin(), rssList.end(), [aCategory](const RSSPtr& a) { return aCategory == a->getCategory(); });
	if (r != rssList.end())
		return *r;

	return nullptr;
}

RSSPtr RSSManager::getFeedByUrl(const string& aUrl) {
	Lock l(cs);
	auto r = find_if(rssList.begin(), rssList.end(), [aUrl](const RSSPtr& a) { return aUrl == a->getUrl(); });
	if (r != rssList.end())
		return *r;

	return nullptr;
}

void RSSManager::parseAtomFeed(SimpleXML& xml, RSSPtr& aFeed) {
	xml.stepIn();
		while (xml.findChild("entry")) {
			xml.stepIn();
			bool newdata = false;
			string titletmp;
			string link;
			string date;

			if (xml.findChild("link")) {
				link = xml.getChildAttrib("href");
			}
			if (xml.findChild("title")) {
				titletmp = xml.getChildData();
				newdata = checkTitle(aFeed, titletmp);
			}
			if (xml.findChild("updated"))
				date = xml.getChildData();

			if (newdata) 
				addData(titletmp, link, date, aFeed);

			xml.stepOut();
		}
	xml.stepOut();
}

void RSSManager::parseRSSFeed(SimpleXML& xml, RSSPtr& aFeed) {
	xml.stepIn();
	if (xml.findChild("channel")) {
		xml.stepIn();
		while (xml.findChild("item")) {
			xml.stepIn();
			bool newdata = false;
			string titletmp;
			string link;
			string date;
			if (xml.findChild("title")) {
				titletmp = xml.getChildData();
				newdata = checkTitle(aFeed, titletmp);
			}

			if (xml.findChild("link")) {
				link = xml.getChildData();
				//temp fix for some urls
				if (strncmp(link.c_str(), "//", 2) == 0)
					link = "https:" + link;
			}
			if (xml.findChild("pubDate"))
				date = xml.getChildData();


			if (newdata)
				addData(titletmp, link, date, aFeed);

			xml.stepOut();
		}
		xml.stepOut();
	}
	xml.stepOut();
}

void RSSManager::downloadComplete(const string& aUrl) {
	auto feed = getFeedByUrl(aUrl);
	if (!feed)
		return;

	auto& conn = feed->rssDownload;
	ScopedFunctor([&conn] { conn.reset(); });

	if (conn->buf.empty()) {
		LogManager::getInstance()->message(conn->status, LogMessage::SEV_ERROR);
		return;
	}

	string tmpdata(conn->buf);
	string erh;
	string type;
	unsigned long i = 1;
	while (i) {
		unsigned int res = 0;
		sscanf(tmpdata.substr(i-1,4).c_str(), "%x", &res);
		if (res == 0){
			i=0;
		}else{
			if (tmpdata.substr(i-1,3).find("\x0d") != string::npos)
				erh += tmpdata.substr(i+3,res);
			if (tmpdata.substr(i-1,4).find("\x0d") != string::npos)
				erh += tmpdata.substr(i+4,res);
			else
				erh += tmpdata.substr(i+5,res);
			i += res+8;
		}
	}
	try {
		SimpleXML xml;
		xml.fromXML(tmpdata.c_str());
		if(xml.findChild("rss")) {
			parseRSSFeed(xml, feed);
		}
		xml.resetCurrentChild();
		if (xml.findChild("feed")) {
			parseAtomFeed(xml, feed);
		}
	} catch(const Exception& e) {
		LogManager::getInstance()->message(e.getError().c_str(), LogMessage::SEV_ERROR);
	}
}

bool RSSManager::checkTitle(const RSSPtr& aFeed, string& aTitle) {
	if (aTitle.empty())
		return false;
	boost::algorithm::trim_if(aTitle, boost::is_space() || boost::is_any_of("\r\n"));
	Lock l(cs);
	return aFeed->getFeedData().find(aTitle) == aFeed->getFeedData().end();
}

void RSSManager::addData(const string& aTitle, const string& aLink, const string& aDate, RSSPtr& aFeed) {
	RSSDataPtr data = new RSSData(aTitle, aLink, aDate, aFeed);
	matchFilters(data);
	{
		Lock l(cs);
		aFeed->getFeedData().emplace(aTitle, data);
	}
	fire(RSSManagerListener::RSSDataAdded(), data);
}

void RSSManager::matchFilters(const RSSPtr& aFeed) {
	if (aFeed) {
		Lock l(cs);
		for (auto data : aFeed->getFeedData() | map_values) {
				matchFilters(data);
		}
	}
}

void RSSManager::matchFilters(const RSSDataPtr& aData) {
	
	for (auto& aF : rssFilterList) {
		if (AirUtil::stringRegexMatch(aF.getFilterPattern(), aData->getTitle())) {

			auto targetType = TargetUtil::TargetType::TARGET_PATH;
			AutoSearchManager::getInstance()->addAutoSearch(aData->getTitle(),
				aF.getDownloadTarget(), targetType, true, AutoSearch::RSS_DOWNLOAD, true);

			break; //One match is enough
		}
	}
}

void RSSManager::updateFeedItem(RSSPtr& aFeed, const string& aUrl, const string& aCategory, int aUpdateInterval) {
	auto r = rssList.find(aFeed);
	if (r != rssList.end())
	{
		{
			Lock l(cs);
			aFeed->setUrl(aUrl);
			aFeed->setCategory(aCategory);
			aFeed->setUpdateInterval(aUpdateInterval);
		}
		fire(RSSManagerListener::RSSFeedChanged(), aFeed);
	} else {
		{
			Lock l(cs);
			rssList.emplace(aFeed);
		}
		fire(RSSManagerListener::RSSFeedAdded(), aFeed);
	}
}

void RSSManager::updateFilterList(vector<RSSFilter>& aNewList) {
	Lock l(cs);
	rssFilterList = aNewList;
}

void RSSManager::removeFeedItem(const RSSPtr& aFeed) {
	Lock l(cs);
	rssList.erase(aFeed);
	fire(RSSManagerListener::RSSFeedRemoved(), aFeed);
}

void RSSManager::downloadFeed(const RSSPtr& aFeed) {
	if (!aFeed)
		return;

	string url = aFeed->getUrl();
	aFeed->setLastUpdate(GET_TIME());
	aFeed->rssDownload.reset(new HttpDownload(aFeed->getUrl(),
		[this, url] { downloadComplete(url); }, false));

	fire(RSSManagerListener::RSSFeedUpdated(), aFeed);
	LogManager::getInstance()->message("updating the " + aFeed->getUrl(), LogMessage::SEV_INFO);
}

RSSPtr RSSManager::getUpdateItem() {
	for (auto i : rssList) {
		if (i->allowUpdate())
			return i;
	}
	return nullptr;
}


void RSSManager::on(TimerManagerListener::Second, uint64_t aTick) noexcept {
	if (rssList.empty())
		return;

	if (nextUpdate < aTick) {
		Lock l(cs);
		downloadFeed(getUpdateItem());
		nextUpdate = GET_TICK() + 1 * 60 * 1000; //Minute between item updates for now, TODO: handle intervals smartly :)
	}
}

void RSSManager::load() {

	SimpleXML xml;
	SettingsManager::loadSettingFile(xml, CONFIG_DIR, CONFIG_NAME);
	if (xml.findChild("RSS")) {
		xml.stepIn();

		while (xml.findChild("Settings")) {
			auto feed = std::make_shared<RSS>(xml.getChildAttrib("Url"),
				xml.getChildAttrib("Categorie"),
				Util::toInt64(xml.getChildAttrib("LastUpdate")),
				xml.getIntChildAttrib("UpdateInterval"));
			xml.stepIn();
			loaddatabase(feed, xml);
			xml.stepOut();
			rssList.emplace(feed);
		}
		xml.resetCurrentChild();
		while (xml.findChild("Filter")) {
			rssFilterList.emplace_back(
				xml.getChildAttrib("FilterPattern"),
				xml.getChildAttrib("DownloadTarget"));
		}

		xml.stepOut();
	}
	
	TimerManager::getInstance()->addListener(this);
	nextUpdate = GET_TICK() + 10 * 1000; //start after 10 seconds
}

void RSSManager::loaddatabase(const RSSPtr& aFeed, SimpleXML& aXml) {
	if (aXml.findChild("Data")) {
		aXml.stepIn();
		while (aXml.findChild("item")) {

			auto rd = new RSSData(aXml.getChildAttrib("title"),
				aXml.getChildAttrib("link"),
				aXml.getChildAttrib("pubdate"),
				aFeed,
				Util::toInt64(aXml.getChildAttrib("dateadded")));

			aFeed->getFeedData().emplace(rd->getTitle(), rd);
		}
		aXml.stepOut();
	}
}

void RSSManager::save() {
	SimpleXML xml;
	xml.addTag("RSS");
	xml.stepIn();
	for (auto r : rssList) {
		xml.addTag("Settings");
		xml.addChildAttrib("Url", r->getUrl());
		xml.addChildAttrib("Categorie", r->getCategory());
		xml.addChildAttrib("LastUpdate", Util::toString(r->getLastUpdate()));
		xml.addChildAttrib("UpdateInterval", Util::toString(r->getUpdateInterval()));
		xml.stepIn();
		savedatabase(r, xml);
		xml.stepOut();
	}

	for (auto f : rssFilterList) {
		xml.addTag("Filter");
		xml.addChildAttrib("FilterPattern", f.getFilterPattern());
		xml.addChildAttrib("DownloadTarget", f.getDownloadTarget());
	}

	xml.stepOut();

	SettingsManager::saveSettingFile(xml, CONFIG_DIR, CONFIG_NAME);
}

void RSSManager::savedatabase(const RSSPtr& aFeed, SimpleXML& aXml) {
	aXml.addTag("Data");
	aXml.stepIn();
	for (auto r : aFeed->getFeedData() | map_values) {
		//Don't save more than 3 days old entries... Todo: setting?
		if ((r->getDateAdded() + 3 * 24 * 60 * 60) > GET_TIME()) {
			aXml.addTag("item");
			aXml.addChildAttrib("title", r->getTitle());
			aXml.addChildAttrib("link", r->getLink());
			aXml.addChildAttrib("pubdate", r->getPubDate());
			aXml.addChildAttrib("dateadded", Util::toString(r->getDateAdded()));
		}
	}
	aXml.stepOut();
}

}