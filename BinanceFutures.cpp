// Doc: https://binance-docs.github.io/apidocs/futures/en/
// Doc: https://binance-docs.github.io/apidocs/delivery/en/

#include "stdafx.h"
#include <string>
#include <mmsystem.h>
#include <math.h>
#include <ATLComTime.h>

// to use the Sleep function
#include <windows.h>		// Sleep(), in miliseconds
#include <time.h>
//debug log
#include <debugapi.h>
#include <ctime>
#include "source.h"
//#include <unistd.h>
#include <chrono>

typedef double DATE;
#include "trading.h"
#include "jsmn.h"
#include "sha256.h"
#include "base64.h"

//#define DEBUG(a,b) 
#define DEBUG(a,b) showError(a,b);
#define PLUGIN_VERSION 2
#define DLLFUNC extern "C" __declspec(dllexport)
#define SAFE_RELEASE(p) if(p) p->release(); p = NULL

/////////////////////////////////////////////////////////////
#pragma warning(disable : 4996 4244 4312)


BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
	return TRUE;
}

/////////////////////////////////////////////////////////////
const char* NAME = "Binance Futures";

const char* TOKERR = "error";

/////////////////////////////////////////////////////////////
int (__cdecl *BrokerError)(const char *txt) = NULL;
int (__cdecl *BrokerProgress)(const int percent) = NULL;
int (__cdecl *http_send)(const char* url, const char* data, const char* header) = NULL;
long (__cdecl *http_status)(int id) = NULL;
long (__cdecl *http_result)(int id,char* content,long size) = NULL;
int (__cdecl *http_free)(int id) = NULL;

static int loop_ms = 50, wait_ms = 30000;
static BOOL g_bDemoOnly = TRUE;
static BOOL g_bIsDemo = TRUE;
static BOOL g_bConnected = FALSE;
static char g_Account[32] = "", g_Asset[64] = "";
static char g_Uuid[256] = "";
static char g_Password[256] = "", g_Secret[256] = "";
static char g_Command[1024];
static int g_Warned = 0;
static BOOL isForex;
__int64 g_IdOffs64 = 0;
static int g_HttpId = 0;
static int g_nDiag = 0;
static double g_Amount = 1.;
static double g_Limit = 0.;
static int g_VolType = 0;
static int g_Id = 0;
static int g_OrderType = 0;
static bool hedge = false;
static int counterPer1min = 0;
static int countBrokerAsset = 0;
static int countBrokerTrade = 0;
static int countBrokerAccount = 0;
static HWND g_hWindow = NULL;
static char* g_TGToken;
static long g_TGChatId;
static bool g_useTestnet = false;
static bool g_rateLimitTest = false;
static bool g_postMessageTest = false;
static int g_tradeVolumeInterval = 1;
static char* g_orderComments;

#define OBJECTS	300
#define TOKENS		20+OBJECTS*32


void showError(const char* text, const char *detail)
{
	static char msg[4096];
	if (!detail) detail = "";
	sprintf_s(msg, "%s %s", text, detail);
	//	if(strstr(msg,"imit viola")) return;
	BrokerError(msg);
	/*	if(g_nDiag >= 1) {
	sprintf_s(msg,"Cmd: %s",g_Command);
	BrokerError(msg);
	}*/
}
void Log(char* Name, char *var) {
	char msgbuf[1024];
	sprintf(msgbuf, "%s: %s\n", Name, var);
	OutputDebugStringA(msgbuf);
	DEBUG("[DEBUG] ", msgbuf);
}

char* rheader(char* asset) {
	if (!strcmp(asset,"") or strstr(asset, "USDT"))
		return (g_useTestnet) ? "https://testnet.binancefuture.com/fapi/" : "https://fapi.binance.com/fapi/";
	else
		return (g_useTestnet) ? "https://testnet.binancefuture.com/dapi/" : "https://dapi.binance.com/dapi/";
}




char* itoa(int n)
{
	static char buffer[64];
	if(0 != _itoa_s(n,buffer,10)) *buffer = 0;
	return buffer;
}

char* i64toa(__int64 n)
{
	static char buffer[64];
	if(0 != _i64toa_s(n,buffer,64,10)) *buffer = 0;
	return buffer;
}


int atoix(char* c) { return (int)(_atoi64(c)-g_IdOffs64); }

char* ftoa(double f)
{
	static char buffer[64];
	if(f < 1.)
		sprintf(buffer,"%.8f",f);
	else if(f < 30.)
		sprintf(buffer,"%.4f",f);
	else if(f < 300) 
		sprintf(buffer,"%.2f",f); // avoid "invalid precision"
	else 
		sprintf(buffer,"%.0f",f);
	return buffer;
}

double roundto(var Val,var Step)
{
	return Step*(floor(Val/Step+0.5));
}

int sleep(int ms)
{
	Sleep(ms); 
	return BrokerProgress(0);
}

inline BOOL isConnected(int Key = 0)
{
	if(g_bDemoOnly || !g_bConnected) return 0;
	if(Key && (!*g_Password || !*g_Secret)) return 0;
	return 1;
}

DATE convertTime(__int64 t64)
{
	if(t64 == 0) return 0.;
	return (25569. + ((double)(t64/1000))/(24.*60.*60.));
}

__int64 convertTime(DATE Date)
{
	return 1000*(__int64)((Date - 25569.)*24.*60.*60.);
}



// convert ETH/BTC -> ethbtc
char* fixAsset(char* Asset,int Mode = 1)
{
	static char NewAsset[32];
	char* Minus = strchr(Asset,'-');
// convert BTC-ETH -> ethbtc
	if(Minus) {
		strcpy_s(NewAsset,Minus+1);
		strcat_s(NewAsset,Asset);
		Minus = strchr(NewAsset,'-'); 
		*Minus = 0;
	} else {
// convert ETH/BTC -> ETHBTC
		strcpy_s(NewAsset,Asset);
		char* Slash = strchr(NewAsset,'/'); 
		if(Slash) {
			if(Mode == 2)
				*Slash = 0; // -> ETH, for balance
			else
				strcpy_s(Slash,16,Slash+1);
		}
	}
//	if(Lwr) strlwr(NewAsset);
	return NewAsset;
}

const char* getSignature(std::string Post)
{
	std::string PrivateKey = g_Secret;

//Post = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
//PrivateKey = "NhqPtmdSJYdKjVHjA7PZj4Mge3R5YNiP1e3UZjInClVN65XAbvqqM6A7H5fATj0j";
// -> c8db56825ae71d6d79447849e617115f4a920fa2acdcab2b053c4b2838bd6b71

	unsigned char hmac_256[SHA256::DIGEST_SIZE];
	memset(hmac_256, 0, SHA256::DIGEST_SIZE);
	//std::string encp = base64_decode(PrivateKey);
	HMAC256(PrivateKey, (unsigned char *)Post.c_str(), (int)Post.length(), hmac_256);

	static char buf[2 * SHA256::DIGEST_SIZE + 1];
	buf[2 * SHA256::DIGEST_SIZE] = 0;
	for (int i = 0; i < SHA256::DIGEST_SIZE; i++)
		sprintf(buf + i * 2, "%02x", hmac_256[i]);
	
	return buf;
}

static int DoTok = 0;

char* send(const char* dir, const char* param = NULL, int crypt = 0)
{
	if (g_rateLimitTest) {
		counterPer1min++;
		if (counterPer1min % 100 == 0)
			Log("[RateLimitTest: send(): ", itoa(counterPer1min));
	}

	int id;
	strcpy_s(g_Command,rheader(g_Asset));
	strcat_s(g_Command,dir);
	if(crypt) {
		int CmdLength = (int)strlen(g_Command);
		strcat_s(g_Command,"?recvWindow=50000");
		strcat_s(g_Command,"&timestamp=");
		__time64_t Time;
		_time64(&Time);
		strcat_s(g_Command,i64toa(Time*1000));
		if(param)
			strcat_s(g_Command,param);

		char* TotalParams = g_Command+CmdLength+1;
		//TotalParams = "symbol=LTCBTC&side=BUY&type=LIMIT&timeInForce=GTC&quantity=1&price=0.1&recvWindow=5000&timestamp=1499827319559";
		const char* Signature = getSignature(TotalParams); 
		strcat_s(g_Command,"&signature=");
		strcat_s(g_Command,Signature);
		
		char Header[1024]; 
		//strcpy_s(Header,"Content-Type:application/json");
		//strcat_s(Header,"\nAccept:application/json");
		strcpy_s(Header,"X-MBX-APIKEY: ");
		strcat_s(Header,g_Password);
		
		if(crypt == 3)
			id = http_send(g_Command,"#DELETE",Header);
		else if(crypt == 2)
			id = http_send(g_Command,"#POST",Header);
		else
			id = http_send(g_Command,NULL,Header);
	} else {
		if(param) 
			strcat_s(g_Command,param);

		id = http_send(g_Command,NULL,NULL);
	}

	if(crypt >= 2 && g_nDiag >= 2) {
		showError("Send:",g_Command);
	}
	if(!id) return NULL;
	DoTok = 1;	// tokenize again
	int len = http_status(id);
	for(int i=0; i<30*1000/20 && !len; i++) {
		sleep(20); // wait for the server to reply
		len = http_status(id);
	}
	if(len > 0) { //transfer successful?
		static char s[OBJECTS*512];
		int res = http_result(id,s,sizeof(s));
		s[sizeof(s)-1] = 0;
		if(!res || crypt)
			http_free(id);
		else
			g_HttpId = id;
		if(!res) return NULL;
		if(crypt >= 2 && g_nDiag >= 2) {
			showError("Result:",s);
		}
		return s;
	}
	http_free(id);
	g_HttpId = 0;
	return NULL;
}

char* parse(char* str,char* key=NULL)
{
	static char* json = str;
	static jsmn_parser jsmn;
	static jsmntok_t tok[TOKENS];
	static int nTok = 0;
	static int nNext = 0;
	if(str)
		nNext = 0;
	if(str && (DoTok || !key)) {	// tokenize the string
		jsmn_init(&jsmn);
		nTok = jsmn_parse(&jsmn,json,strlen(json),tok,TOKENS);
		if(nTok > 0) DoTok = 0;
		else return "";
	}
	if(key) { // find the correct token
		int KeyLen = (int)strlen(key);
		for(int i=nNext; i<nTok-1; i++) {
			if(tok[i].type == JSMN_STRING && 
				(tok[i+1].type == JSMN_PRIMITIVE || tok[i+1].type == JSMN_STRING)) 
			{
				char* name = json + tok[i].start;
				i++;
				if(0 == strncmp(name,key,KeyLen)) {
					char* s = json + tok[i].start;
					json[tok[i].end] = 0;
					nNext = i+1;	// continue with next token
					return s;
				}
			}
		}
	}
	return "";
}


char* timeFrame(int interval) {
	char *tf = "1m";

	if (1440 <= interval) tf = "1d";
	else if (720 <= interval) tf = "12h";
	else if (480 <= interval) tf = "8h";
	else if (360 <= interval) tf = "6h";
	else if (240 <= interval) tf = "4h";
	else if (120 <= interval) tf = "2h";
	else if (60 <= interval) tf = "1h";
	else if (30 <= interval) tf = "30m";
	else if (15 <= interval) tf = "15m";
	else if (5 <= interval) tf = "5m";
	else if (3 <= interval) tf = "3m";

	return tf;
}
////////////////////////////////////////////////////////////////

DLLFUNC int BrokerOpen(char* Name,FARPROC fpError,FARPROC fpProgress)
{
	if(*Name == '4') {
		g_bDemoOnly = FALSE;
	} else {
		g_bDemoOnly = TRUE;
	}
	strcpy_s(Name,32,NAME);
	(FARPROC&)BrokerError = fpError;
	(FARPROC&)BrokerProgress = fpProgress;

	return PLUGIN_VERSION;
}

DLLFUNC int BrokerHTTP(FARPROC fp_send,FARPROC fp_status,FARPROC fp_result,FARPROC fp_free)
{
	(FARPROC&)http_send = fp_send;
	(FARPROC&)http_status = fp_status;
	(FARPROC&)http_result = fp_result;
	(FARPROC&)http_free = fp_free;

	return 1;
}
////////////////////////////////////////////////////////////////

DLLFUNC int BrokerTime(DATE *pTimeGMT)
{
	if(!isConnected()) return 0;
	return 2;
}


DLLFUNC int BrokerAccount(char* Account,double *pdBalance,double *pdTradeVal,double *pdMarginVal)
{
	if(!isConnected(1)) return 0;
	if (!Account || !*Account) {
		if (!strcmp(g_Asset, "") or strstr(g_Asset, "USDT"))
			Account = "USDT";
		else if (strstr(g_Asset, "ADA"))
			Account = "ADA";
		else if (strstr(g_Asset, "BCH"))
			Account = "BCH";
		else if (strstr(g_Asset, "BNB"))
			Account = "BNB";
		else if (strstr(g_Asset, "BTC"))
			Account = "BTC";
		else if (strstr(g_Asset, "DOT"))
			Account = "DOT";
		else if (strstr(g_Asset, "EOS"))
			Account = "EOS";
		else if (strstr(g_Asset, "ETC"))
			Account = "ETC";
		else if (strstr(g_Asset, "ETH"))
			Account = "ETH";
		else if (strstr(g_Asset, "FIL"))
			Account = "FIL";
		else if (strstr(g_Asset, "LINK"))
			Account = "LINK";
		else if (strstr(g_Asset, "LTC"))
			Account = "LTC";
		else if (strstr(g_Asset, "TRX"))
			Account = "TRX";
		else if (strstr(g_Asset, "XRP"))
			Account = "XRP";
	}

	if (g_rateLimitTest)
	    countBrokerAccount++;

	//weight=5
	char* Response = (!strcmp(Account,"USDT")) ? send("v2/account",0,1) : send("v1/account",0,1);
	if(!Response) return 0;
	parse(Response);
	double Balance = 0;
	double TradeVal = 0;
	double MarginVal = 0;

	while(1) {
		char* Found = parse(NULL,"asset");
		if(!Found || !*Found) break;
		if (!strcmp(Found, Account)) {
			Balance = atof(parse(NULL, "walletBalance")); //  錢包餘額 = balance
			TradeVal = atof(parse(NULL, "unrealizedProfit")); // 未實現盈虧 = equity - balance = P&L
			MarginVal = atof(parse(NULL, "maintMargin")); // 維持保證金 = margin
		}
	}
	
	if(pdBalance) *pdBalance = Balance;
	if(pdTradeVal) *pdTradeVal = TradeVal;
	if(pdMarginVal) *pdMarginVal = MarginVal;


	//weight=1
	//hedge or NFA
	char* Response2 = send("v1/positionSide/dual", 0, 1);
	if (!Response2) return 0;
	parse(Response2);
	while (1) {
		char* Found = parse(NULL, "dualSidePosition");
		if (!Found || !*Found) break;
		if (!strcmp(Found, "true"))
			hedge = true;
		else if (!strcmp(Found, "false"))
			hedge = false;
	}

	return Balance > 0.? 1 : 0;
}




DLLFUNC int BrokerAsset(char* Asset,double* pPrice,double* pSpread,
	double *pVolume, double *pPip, double *pPipCost, double *pMinAmount,
	double *pMarginCost, double *pRollLong, double *pRollShort)
{
	if(!isConnected()) return 0;

	if (g_postMessageTest)
	    Log("[PostMessageTest] ", "BrokerAsset()");

	strcpy_s(g_Asset, fixAsset(Asset)); // for BrokerAsset

	
	//auto start_time = std::chrono::high_resolution_clock::now();
	if (g_rateLimitTest)
	    countBrokerAsset++;
	//weight=1
	//char* Result = send("v1/ticker/bookTicker?symbol=", fixAsset(Asset), 0);
	char* Result = send("v1/ticker/price?symbol=", fixAsset(Asset), 0);
	//auto end_time = std::chrono::high_resolution_clock::now();
	//auto time = end_time - start_time;
	//Log("Wait: ", i64toa(std::chrono::duration_cast<std::chrono::milliseconds>(time).count()));


	if (!Result || !*Result) return 0;
	if (!parse(Result)) return 0;
	double price = atof(parse(Result, "price"));
	if (price == 0.) return 0;
	if (pPrice) *pPrice = price;

	if (pVolume) {


		char* tf = timeFrame(g_tradeVolumeInterval);

		char Command[256] = "?symbol=";
		strcat_s(Command, fixAsset(Asset, 0));
		strcat_s(Command, "&interval=");
		strcat_s(Command, tf);
		strcat_s(Command, "&limit=2");

		//weight=1
		char* Result = send("v1/klines", Command, 0);
		if (!Result || !*Result) return 0;
		Result = strchr(Result, '[');
		if (!Result || !*Result) return 0;


		float Open[2], High[2], Low[2], Close[2], Volume[2];
		__int64 TimeOpen[2], TimeClose[2];
		float QuoteVolume[2], TakeBuyVolume[2], TakerbuyQuoteVolume[2];
		int NumberOfTrades[2];
		__time64_t Time;
		_time64(&Time);
		int i = 0;
		for (; i < 2; i++) {
			Result = strchr(++Result, '[');
			sscanf(Result, "[%I64d,\"%f\",\"%f\",\"%f\",\"%f\",\"%f\",%I64d,\"%f\",%d,\"%f\",\"%f\",",
				&TimeOpen[i], &Open[i], &High[i], &Low[i], &Close[i], &Volume[i], &TimeClose[i],
				&QuoteVolume[i], &NumberOfTrades[i], &TakeBuyVolume[i], &TakerbuyQuoteVolume[i]);
		}

		if (Time * 1000 > TimeClose[0] && Time * 1000 < TimeClose[1]) {
			*pVolume = Volume[0];
		}
		else if (Time * 1000 > TimeClose[0] && Time * 1000 > TimeClose[1]) {
			*pVolume = Volume[1];
		}

		if (g_rateLimitTest) {
			Log("[RateLimitTest] send(): ", itoa(counterPer1min));
			Log("[RateLimitTest] BrokerAccount(): ", itoa(countBrokerAccount));
			Log("[RateLimitTest] BrokerAsset(): ", itoa(countBrokerAsset));
			Log("[RateLimitTest] BrokerTrade(): ", itoa(countBrokerTrade));
			counterPer1min = 0;
			countBrokerAccount = 0;
			countBrokerAsset = 0;
			countBrokerTrade = 0;
		}
	}
	return 1;
}	

DLLFUNC int BrokerHistory2(char* Asset,DATE tStart,DATE tEnd,int nTickMinutes,int nTicks,T6* ticks)
{
	if(!isConnected()) return 0;
	if(!ticks || !nTicks) return 0;

	
	char* tf = timeFrame(nTickMinutes);
	

	char msgbuf[100];
	
	sprintf(msgbuf, "Asset: %s\n", Asset);
	OutputDebugStringA(msgbuf);
	sprintf(msgbuf, "nTickMinutes: %s\n", itoa(nTickMinutes));
	OutputDebugStringA(msgbuf);

	SYSTEMTIME systime = {0};
	VariantTimeToSystemTime(tStart, &systime);

	sprintf(msgbuf,"tStart:(DATE:%lf)(Timestamp:%s) %04u/%02u/%02u %02d:%02d:%02d", tStart, i64toa(convertTime(tStart)), systime.wYear, systime.wMonth, systime.wDay, systime.wHour, systime.wMinute, systime.wSecond);
	OutputDebugStringA(msgbuf);
	DEBUG("[DEBUG] Start:", msgbuf);

	VariantTimeToSystemTime(tEnd, &systime);
	sprintf(msgbuf, "tEnd:(DATE:%lf)(Timestamp:%s) %04u/%02u/%02u %02d:%02d:%02d", tEnd, i64toa(convertTime(tEnd)),systime.wYear, systime.wMonth, systime.wDay, systime.wHour, systime.wMinute, systime.wSecond);
	OutputDebugStringA(msgbuf);
	DEBUG("[DEBUG] End:", msgbuf);

	nTicks = 1000; //1000 bars limit per request
    sprintf(msgbuf, "nTicks: %s\n", itoa(nTicks));
	OutputDebugStringA(msgbuf);

	__int64 TTStart = convertTime(tStart);
	__int64 TTEnd = convertTime(tEnd);
	int allTicks = 0;
	int count = 0;


	while (TTStart < TTEnd) {
		char Command[256] = "?symbol=";
		strcat_s(Command, fixAsset(Asset, 0));
		strcat_s(Command, "&interval=");
		strcat_s(Command, tf);
		strcat_s(Command, "&limit=");   //默?值:500 最大值:1500.
		strcat_s(Command, itoa(nTicks));
		strcat_s(Command, "&startTime=");
		strcat_s(Command, i64toa(TTStart));
		strcat_s(Command, "&endTime=");
		strcat_s(Command, i64toa(TTEnd));

		//weight=1
		char* Result = send("v1/klines", Command, 0);
		if (!Result || !*Result) goto raus;
		Result = strchr(Result, '[');
		if (!Result || !*Result) goto raus;
		int i = 0;
		for (; i < nTicks; i++, ticks++) {
			Result = strchr(++Result, '[');
			if (!Result || !*Result) break;
			__int64 TimeOpen, TimeClose;
			float QuoteVolume;
			int NumberOfTrades;
			float TakerbuyVolume;
			float TakerbuyQuoteVolume;
			sscanf(Result, "[%I64d,\"%f\",\"%f\",\"%f\",\"%f\",\"%f\",%I64d,\"%f\",%d,\"%f\",\"%f\",",
				&TimeOpen, &ticks->fOpen, &ticks->fHigh, &ticks->fLow, &ticks->fClose, &ticks->fVol, &TimeClose,
				&QuoteVolume, &NumberOfTrades, &ticks->fVal, &TakerbuyQuoteVolume);
			ticks->time = convertTime(TimeClose);

			if (i == 0 && TimeOpen>TTStart) { //startTime沒data，中間有data
				TTStart = TimeOpen;
			}
		}

		if (i==0) goto raus; //no data

		allTicks += i;
		TTStart += 1000LL * 60LL * nTickMinutes * i; //下載i個bar經過多少ms

		//int allTicksPre = (TTEnd - TTStart) / 1000LL / 60LL / nTickMinutes;

		SYSTEMTIME systime = {0};
		VariantTimeToSystemTime(convertTime(TTStart), &systime);

		sprintf(msgbuf, "[%d] allTicks:%s, Next: %04u/%02u/%02u %02d:%02d:%02d\n", count++, itoa(allTicks), systime.wYear, systime.wMonth, systime.wDay, systime.wHour, systime.wMinute, systime.wSecond);
		OutputDebugStringA(msgbuf);
		DEBUG("[DEBUG]",msgbuf);
		//sleep(25); //fapi ratelimits: 2400/1m = 25ms
	}

	DEBUG("[DEBUG] Mio Get allTicks:", itoa(allTicks));

	return allTicks;
raus:
	if(g_Warned++ <= 1) showError(Asset,"no data");
	return 0;
}



// returns negative amount when the trade was closed
DLLFUNC int BrokerTrade(int nTradeID,double *pOpen,double *pClose,double *pRoll,double *pProfit)
{
	if(!isConnected(1)) return 0;


	char Param[512] = "&symbol=";
	strcat_s(Param,g_Asset);
	strcat_s(Param,"&origClientOrderId=");
	strcat_s(Param,itoa(nTradeID));


	if (g_rateLimitTest)
	    countBrokerTrade++;

	//weight=1
	char* Result = send("v1/order",Param,1);
	if(!Result || !*Result) return 0;
	if(!strstr(Result,"clientOrderId")) return 0;
	if(!parse(Result)) return 0;
	double openPrice = atof(parse(Result,"avgPrice"));
	char* side = parse(Result, "side");
	if(pOpen) *pOpen = openPrice;
	double executedQty = atof(parse(Result, "executedQty"));
	int Fill = (int)(executedQty * 1000) / (int)(g_Amount * 1000);






	double currentPrice = 0;
	BrokerAsset(g_Asset,&currentPrice,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
	if (pClose) *pClose = currentPrice;


	if (pProfit) {
		//weught = 1
		char* Result2 = (strstr(g_Asset, "USDT")) ? send("v2/positionRisk", 0, 1) : send("v1/positionRisk", 0, 1);
		//char* Result2 = (strstr(g_Asset, "USDT")) ? send("v2/account", 0, 1) : send("v1/account", 0, 1);
		if (!Result2 || !*Result2) return 0;
		if (!parse(Result2)) return 0;
		while (1) {
			char* Found = parse(NULL, "symbol");
			if (!Found || !*Found) break;
			if (!strcmp(Found, g_Asset)) {
				*pProfit += atof(parse(NULL, "unRealizedProfit"));   //for v2/positionRisk
				//*pProfit += atof(parse(NULL, "unrealizedProfit")); //for v2/account
			}
		}
	}

/*	
	if (pProfit) {
		if (strstr(g_Asset, "USDT")) {
			if (!strcmp(side, "BUY"))
				*pProfit = (currentPrice - openPrice) * executedQty;
			else if (!strcmp(side, "SELL"))
				*pProfit = (openPrice - currentPrice) * executedQty;
		}
		else {
			char* Result3 = send("v1/account", 0, 1);
			if (!Result3 || !*Result3) return 0;
			if (!parse(Result3)) return 0;
			while (1) {
				char* Found = parse(NULL, "symbol");
				if (!Found || !*Found) break;
				if (!strcmp(Found, g_Asset)) {
					*pProfit = atof(parse(NULL, "unrealizedProfit"));
				}
			}
		}

	}
	*/
/*
	if (pProfit) {
		if (!strcmp(g_Account,"USDT")) {
			if (!strcmp(side, "BUY"))
				*pProfit = (currentPrice - openPrice) * executedQty;
			else if (!strcmp(side, "SELL"))
				*pProfit = (openPrice - currentPrice) * executedQty;
		}
		else {
			if (!strcmp(side, "BUY"))
				*pProfit = currentPrice/((currentPrice - openPrice) * executedQty);
			else if (!strcmp(side, "SELL"))
				*pProfit = currentPrice/((openPrice - currentPrice) * executedQty);
		}


	}
*/



	return Fill;
}

DLLFUNC int BrokerBuy2(char* Asset,int Amount,double dStopDist,double Limit,double *pPrice,int *pFill)
{
	char message[1024];
	if(!isConnected(1)) return 0;
	strcpy_s(g_Asset,fixAsset(Asset)); // for BrokerTrade

	char Param[512] = "&symbol=";
	strcat_s(Param,g_Asset);
	strcat_s(Param,"&side=");
	Amount > 0 ? strcat_s(Param,"BUY") : strcat_s(Param,"SELL");
	//positionSide
	strcat_s(Param, "&positionSide=");
	hedge ?	(Amount > 0 ? strcat_s(Param, "LONG") : strcat_s(Param, "SHORT")) : strcat_s(Param, "BOTH");
	strcat_s(Param,"&type=");
	if(Limit > 0.) {
		double TickSize = 0.0000001;
		Limit = ((int)(Limit/ g_Amount))*g_Amount;	// clip to integer multiple of tick size
		strcat_s(Param,"LIMIT");
		strcat_s(Param,"&price=");
		strcat_s(Param,ftoa(Limit));
		strcat_s(Param,"&timeInForce=");
		if(g_OrderType == 1)
			strcat_s(Param,"IOC");
		else if(g_OrderType == 2)
			strcat_s(Param,"GTC");
		else
			strcat_s(Param,"FOK");
	} else
		strcat_s(Param,"MARKET");
	strcat_s(Param,"&newOrderRespType=RESULT");
	strcat_s(Param,"&quantity=");
		
	//Log("Lots", itoa(Amount));
	//Log("LotAmount", ftoa(g_Amount));
	 
	double TickSize = (strstr(g_Asset, "USDT")) ? 0.0000001 : 1. ;
	double TotalAmount = ((int)(g_Amount*abs(Amount)/ TickSize))*TickSize;

    strcat_s(Param,ftoa(TotalAmount));
	strcat_s(Param,"&newClientOrderId=");
	strcat_s(Param,itoa(g_Id++));
	//weight=1
	char* Result = send("v1/order",Param,2);
	if(!Result || !*Result) {
		showError(Param,"- no result");

		if (g_TGToken != 0 && g_TGChatId != 0) {
			sprintf(message, "[Error] %s - no result", Param);
			send_message(g_TGChatId, message);
		}
		return 0;
	}

	////////////////////////////////////////////////

	if(!strstr(Result,"clientOrderId")) {
		showError(Param,Result);
		if (g_TGToken != 0 && g_TGChatId != 0) {
			sprintf(message, "[Error] %s %s", Param, Result);
			send_message(g_TGChatId, message);
		}
		return 0;
	}
	if(!parse(Result)) {
		showError(Result,"- invalid");
		if (g_TGToken != 0 && g_TGChatId != 0) {
			sprintf(message, "[Error] %s - invalid", Result);
			send_message(g_TGChatId, message);
		}
		return 0;
	}


	int Id = atoi(parse(Result,"clientOrderId"));
	char* executedQty = parse(Result, "executedQty");
	int Fill = (int)(atof(executedQty)*1000)/ (int)(g_Amount*1000);
	
	char* avgPrice = parse(Result, "avgPrice");
	double Price = atof(avgPrice);

	if(pPrice && Price > 0.) *pPrice = Price; // test

	if(pFill) *pFill = Fill;


	if (g_TGToken != 0 && g_TGChatId != 0) {
	    sprintf(message, (Amount > 0)? "<i>**Buy order placed**</i>\n" : "<i>**Sell order placed**</i>\n");
		sprintf(message, "%sSymbol: <b>%s(%s)</b>\n", message, g_Asset, timeFrame(g_tradeVolumeInterval));
		sprintf(message, "%sPrice: <b>%s</b>\n", message, avgPrice);
		sprintf(message, "%sSide: <b>%s</b>\n", message, (Amount > 0) ? "BUY":"SELL");
		sprintf(message, "%sExecutedQty: <b>%s</b>\n", message, executedQty);
		sprintf(message, "%sOrigQty: <b>%s</b>\n", message, ftoa(TotalAmount));
		sprintf(message, "%sType: <b>%s</b>\n", message, (Limit > 0.) ? "LIMIT":"MARKET");
		sprintf(message, "%sclientOrderId: <b>%d</b>\n", message, Id);
		sprintf(message, "%sAccount: <b>%s</b>", message, g_orderComments);
		//Log("", message);
		setup_bot(g_TGToken);
		send_message(g_TGChatId, message);
	}

	if(g_OrderType == 2 || Fill == Amount)
		return Id; // fully filled or GTC
	if(Fill)
		return Id; // IOC partially or FOK fully filled

	return 0;
}

// For NFA=0=雙開模式平倉用
DLLFUNC int BrokerSell2(int nTradeID, int nAmount, double Limit, double *pClose, double *pCost, double *pProfit, int *pFill)
{
	if (!isConnected(1)) return 0;

	char message[1024];
	if (!hedge) {
		char Param[512] = "&symbol=";
		strcat_s(Param, g_Asset);
		strcat_s(Param, "&side=");
		(-nAmount > 0) ? strcat_s(Param, "BUY") : strcat_s(Param, "SELL");
		strcat_s(Param, "&positionSide=");
		strcat_s(Param, "BOTH");
		strcat_s(Param, "&type=");
		if (Limit > 0.) {
			double TickSize = 0.0000001;
			Limit = ((int)(Limit / TickSize))*TickSize;	// clip to integer multiple of tick size
			strcat_s(Param, "LIMIT");
			strcat_s(Param, "&price=");
			strcat_s(Param, ftoa(Limit));
			strcat_s(Param, "&timeInForce=");
			if (g_OrderType == 1)
				strcat_s(Param, "IOC");
			else if (g_OrderType == 2)
				strcat_s(Param, "GTC");
			else
				strcat_s(Param, "FOK");
		}
		else
			strcat_s(Param, "MARKET");

		strcat_s(Param, "&newOrderRespType=RESULT");
		strcat_s(Param, "&quantity=");
		double TickSize = (strstr(g_Asset,"USDT")) ? 0.0000001 : 1.;
		double TotalAmount = ((int)(g_Amount*abs(nAmount) / TickSize))*TickSize;
		strcat_s(Param, ftoa(TotalAmount));
		strcat_s(Param, "&newClientOrderId=");
		strcat_s(Param, itoa(g_Id++));
		strcat_s(Param, "&reduceOnly=true"); //僅減倉
		//weight=1
		char* Result = send("v1/order", Param, 2);
		if (!Result || !*Result) {
			showError(Param, "- no result");
			return 0;
		}

		if (!strstr(Result, "clientOrderId")) {
			showError(Param, Result);
			return 0;
		}
		if (!parse(Result)) {
			showError(Result, "- invalid");
			return 0;
		}
		int Id = atoi(parse(Result, "clientOrderId"));

		char* executedQty = parse(Result, "executedQty");
		int Fill = (int)(atof(executedQty) * 1000) / (int)(g_Amount * 1000);

		char* avgPrice = parse(Result, "avgPrice");
		double Price = atof(avgPrice);

		if (pClose && Price > 0.) *pClose = Price;
		if (pFill) *pFill = Fill;


		if (g_TGToken != 0 && g_TGChatId != 0) {
			sprintf(message, (-nAmount > 0) ? "<i>**Close Sell order placed**</i>\n" : "<i>**Close Buy order placed**</i>\n");
			sprintf(message, "%sSymbol: <b>%s(%s)</b>\n", message, g_Asset, timeFrame(g_tradeVolumeInterval));
			sprintf(message, "%sPrice: <b>%s</b>\n", message, avgPrice);
			sprintf(message, "%sSide: <b>%s</b>\n", message, (-nAmount > 0) ? "BUY" : "SELL");
			sprintf(message, "%sExecutedQty: <b>%s</b>\n", message, executedQty);
			sprintf(message, "%sOrigQty: <b>%s</b>\n", message, ftoa(TotalAmount));
			sprintf(message, "%sType: <b>%s</b>\n", message, (Limit > 0.) ? "LIMIT" : "MARKET");
			sprintf(message, "%sclientOrderId: <b>%d</b>\n", message, Id);
			sprintf(message, "%sOrigClientOrderId: <b>%d</b>\n", message, nTradeID);
			sprintf(message, "%sAccount: <b>%s</b>", message, g_orderComments);
			//Log("", message);
			setup_bot(g_TGToken);
			send_message(g_TGChatId, message);
		}


		if (g_OrderType == 2 || Fill == -nAmount)
			return nTradeID; // fully filled or GTC
		if (Fill)
			return Id; // IOC partially or FOK fully filled
	}
	else {
		char Param[512] = "&symbol=";
		strcat_s(Param, g_Asset);
		strcat_s(Param, "&side=");
		(nAmount > 0) ? strcat_s(Param, "SELL") : strcat_s(Param, "BUY");
		strcat_s(Param, "&positionSide=");
		(nAmount > 0) ? strcat_s(Param, "LONG") : strcat_s(Param, "SHORT");
		strcat_s(Param, "&type=");
		if (Limit > 0.) {
			double TickSize = 0.0000001;
			Limit = ((int)(Limit / TickSize))*TickSize;	// clip to integer multiple of tick size
			strcat_s(Param, "LIMIT");
			strcat_s(Param, "&price=");
			strcat_s(Param, ftoa(Limit));
			strcat_s(Param, "&timeInForce=");
			if (g_OrderType == 1)
				strcat_s(Param, "IOC");
			else if (g_OrderType == 2)
				strcat_s(Param, "GTC");
			else
				strcat_s(Param, "FOK");
		}
		else
			strcat_s(Param, "MARKET");

		strcat_s(Param, "&newOrderRespType=RESULT");
		strcat_s(Param, "&quantity=");
		double TickSize = (strstr(g_Asset,"USDT")) ? 0.0000001 : 1.;
		double TotalAmount = ((int)(g_Amount*abs(nAmount) / TickSize))*TickSize;
		strcat_s(Param, ftoa(TotalAmount));
		strcat_s(Param, "&newClientOrderId=");
		strcat_s(Param, itoa(g_Id++));
		//weight=1
		char* Result = send("v1/order", Param, 2);
		if (!Result || !*Result) {
			showError(Param, "- no result");
			return 0;
		}

		if (!strstr(Result, "clientOrderId")) {
			showError(Param, Result);
			return 0;
		}
		if (!parse(Result)) {
			showError(Result, "- invalid");
			return 0;
		}
		int Id = atoi(parse(Result, "clientOrderId"));

		char* executedQty = parse(Result, "executedQty");
		int Fill = (int)(atof(executedQty) * 1000) / (int)(g_Amount * 1000);

		char* avgPrice = parse(Result, "avgPrice");
		double Price = atof(avgPrice);

		if (pClose && Price > 0.) *pClose = Price;
		if (pFill) *pFill = Fill;


		if (g_TGToken != 0 && g_TGChatId != 0) {
			sprintf(message, (-nAmount > 0) ? "<i>**Close Sell order placed**</i>\n" : "<i>**Close Buy order placed**</i>\n");
			sprintf(message, "%sSymbol: <b>%s(%s)</b>\n", message, g_Asset, timeFrame(g_tradeVolumeInterval));
			sprintf(message, "%sPrice: <b>%s</b>\n", message, avgPrice);
			sprintf(message, "%sSide: <b>%s</b>\n", message, (-nAmount > 0) ? "BUY" : "SELL");
			sprintf(message, "%sExecutedQty: <b>%s</b>\n", message, executedQty);
			sprintf(message, "%sOrigQty: <b>%s</b>\n", message, ftoa(TotalAmount));
			sprintf(message, "%sType: <b>%s</b>\n", message, (Limit > 0.) ? "LIMIT" : "MARKET");
			sprintf(message, "%sclientOrderId: <b>%d</b>\n", message, Id);
			sprintf(message, "%sOrigClientOrderId: <b>%d</b>\n", message, nTradeID);
			sprintf(message, "%sAccount: <b>%s</b>", message, g_orderComments);
			//Log("", message);
			setup_bot(g_TGToken);
			send_message(g_TGChatId, message);
		}


		if (g_OrderType == 2 || Fill == nAmount)
			return nTradeID; // fully filled or GTC
		if (Fill)
			return Id; // IOC partially or FOK fully filled

	}
	return 0;
}



DLLFUNC int BrokerLogin(char* User,char* Pwd,char* Type,char* Account)
{
	if(User) {
		if(g_bDemoOnly) {
			showError("Need Zorro S for Binance","");
			return 0;
		}
		g_Warned = 0;
		strcpy_s(g_Password,User);
		strcpy_s(g_Secret,Pwd);
		g_bConnected = 1;
		time_t Time;
		time(&Time);
		g_Id = (int)Time;
		if(!*User || !*Pwd) {
			showError("Price data only","");
		} else 
			/*if(!BrokerAccount(Account,NULL,NULL,NULL))
			return 0;*/

		return 1;
	} else {
		if (g_HttpId)
			http_free(g_HttpId);
		g_HttpId = 0;
		*g_Password = 0;
		*g_Secret = 0;
	}
	return 0;
}

//////////////////////////////////////////////////////////////
DLLFUNC double BrokerCommand(int command,DWORD parameter)
{
	switch(command) {
		case SET_DELAY: loop_ms = parameter;
		case GET_DELAY: return loop_ms;
		case SET_WAIT: wait_ms = parameter;
		case GET_WAIT: return wait_ms;
		case GET_COMPLIANCE: return 2; //可以改
		case SET_DIAGNOSTICS: g_nDiag = parameter; return 1;
		//case SET_LIMIT: g_Limit = *(double*)parameter; return 1; 
		case SET_AMOUNT: g_Amount = *(double*)parameter; return 1; //LotAmount
		case GET_MAXREQUESTS: return 1;
		case GET_MAXTICKS: return 5256000; //1440*365*10  10 years
		case SET_ORDERTYPE: 
			return g_OrderType = parameter;

		case SET_SYMBOL: { 
			char* Asset = fixAsset((char*)parameter);
			if(!Asset || !*Asset) return 0;
			strcpy_s(g_Asset, Asset); 
			return 1;
		}

		case GET_POSITION: { 
			double Balance = 0;
			char* Asset = fixAsset((char*)parameter);
			int Len = (int)strlen(Asset);
			if(Len > 5) Asset[Len-3] = 0; // clip trailing "BTC"
			BrokerAccount(Asset,&Balance,NULL,NULL);
			return Balance;
		}
		
		case GET_BROKERZONE: return 0; //return 0 for UTC

		case DO_CANCEL: {
			if(!isConnected(1)) return 0;
			char Param[512] = "&symbol=";
			strcat_s(Param,g_Asset);
			strcat_s(Param,"&origClientOrderId=");
			strcat_s(Param,itoa(parameter));
			//weight=1
			char* Result = send("v1/order",Param,3);
			if(!Result || !*Result) return 0;
			return 1;
		}

		case GET_BOOK: {
/*
			T2* Quotes = (T2*)parameter;
			if(!Quotes) return 0;
			char* Result =	send("public/getmarketsummary?market=",g_Asset,1);
			if(Result && *Result)
				Quotes[0].time = convertTime(parse(Result,"TimeStamp"));
			else
				return 0;
			int N = 0;
			Result = send("public/getorderbook?type=sell&market=",g_Asset,0);
			if(!Result) return N;
			char* Success = parse(Result,"success");
			if(!strstr(Success,"true")) return N;
			for(; N<MAX_QUOTES/2; N++) {
				Quotes[N].fVol = atof(parse(NULL,"Quantity"));
				Quotes[N].fVal = atof(parse(NULL,"Rate"));	// ask
				if(Quotes[N].fVal == 0.) 
					break;
				Quotes[N].time = Quotes[0].time;
			}
			Result = send("public/getorderbook?type=buy&market=",g_Asset,0);
			if(!Result) return N;
			Success = parse(Result,"success");
			if(!strstr(Success,"true")) return N;
			for(; N<MAX_QUOTES-1; N++) {
				Quotes[N].fVol = atof(parse(NULL,"Quantity"));
				Quotes[N].fVal = -atof(parse(NULL,"Rate"));	// bid
				if(Quotes[N].fVal == 0.) 
					break;
				Quotes[N].time = Quotes[0].time;
			}
			Quotes[N].fVol = Quotes[N].fVal = 0.f;
			Quotes[N].time = 0.; // end mark
			return N;
*/
			return 0;
		}
		case GET_PRICETYPE: return 0;
		case SET_PRICETYPE: return 0;
		case GET_VOLTYPE: return 0;
		case SET_VOLTYPE: return 0;
		case SET_HWND: g_hWindow = (HWND)parameter; return 1;
		case SET_TGTOKEN: g_TGToken = (char *)parameter; return 1; 
		case SET_TGCHATID: g_TGChatId = (long)parameter; return 1;
		case SET_USETESTNET: g_useTestnet = (bool)parameter; return 1;
		case POSTMESSAGE_TEST:{
			if (g_hWindow) {
				Log("Mio: PostMessage() test: ", g_Asset);
				g_postMessageTest = true;
				PostMessage(g_hWindow, WM_APP + 1, 0, 0);
			}
			return 1;
		}
		case RATELIMIT_TEST: g_rateLimitTest = (bool)parameter; return 1;
		case SET_TRADEVOLINTERVAL: g_tradeVolumeInterval = (int)parameter; return 1;
		case SET_COMMENT: g_orderComments = (char *)parameter; return 1;
	}

	return 0.;
}


