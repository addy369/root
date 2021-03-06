// $Id$
// Author: Sergey Linev   21/12/2013

/*************************************************************************
 * Copyright (C) 1995-2013, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

#include "THttpServer.h"

#include "TThread.h"
#include "TTimer.h"
#include "TSystem.h"
#include "TROOT.h"
#include "TUrl.h"
#include "TClass.h"
#include "RVersion.h"
#include "RConfigure.h"
#include "TRegexp.h"

#include "THttpEngine.h"
#include "THttpWSEngine.h"
#include "THttpLongPollEngine.h"
#include "THttpWSHandler.h"
#include "TRootSniffer.h"
#include "TRootSnifferStore.h"
#include "TCivetweb.h"
#include "TFastCgi.h"

#include <string>
#include <cstdlib>
#include <stdlib.h>
#include <string.h>
#include <fstream>

////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// THttpTimer                                                           //
//                                                                      //
// Specialized timer for THttpServer                                    //
// Provides regular calls of THttpServer::ProcessRequests() method      //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

class THttpTimer : public TTimer {
public:
   THttpServer &fServer; ///!< server processing requests

   /// constructor
   THttpTimer(Long_t milliSec, Bool_t mode, THttpServer &serv) : TTimer(milliSec, mode), fServer(serv) {}

   /// timeout handler
   /// used to process http requests in main ROOT thread
   virtual void Timeout() { fServer.ProcessRequests(); }
};

//////////////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////
//                                                                      //
// THttpServer                                                          //
//                                                                      //
// Online http server for arbitrary ROOT application                    //
//                                                                      //
// Idea of THttpServer - provide remote http access to running          //
// ROOT application and enable HTML/JavaScript user interface.          //
// Any registered object can be requested and displayed in the browser. //
// There are many benefits of such approach:                            //
//     * standard http interface to ROOT application                    //
//     * no any temporary ROOT files when access data                   //
//     * user interface running in all browsers                         //
//                                                                      //
// Starting HTTP server                                                 //
//                                                                      //
// To start http server, at any time  create instance                   //
// of the THttpServer class like:                                       //
//    serv = new THttpServer("http:8080");                              //
//                                                                      //
// This will starts civetweb-based http server with http port 8080.     //
// Than one should be able to open address "http://localhost:8080"      //
// in any modern browser (IE, Firefox, Chrome) and browse objects,      //
// created in application. By default, server can access files,         //
// canvases and histograms via gROOT pointer. All such objects          //
// can be displayed with JSROOT graphics.                               //
//                                                                      //
// At any time one could register other objects with the command:       //
//                                                                      //
// TGraph* gr = new TGraph(10);                                         //
// gr->SetName("gr1");                                                  //
// serv->Register("graphs/subfolder", gr);                              //
//                                                                      //
// If objects content is changing in the application, one could         //
// enable monitoring flag in the browser - than objects view            //
// will be regularly updated.                                           //
//                                                                      //
// More information: https://root.cern/root/htmldoc/guides/HttpServer/HttpServer.html  //
//                                                                      //
//////////////////////////////////////////////////////////////////////////

ClassImp(THttpServer);

////////////////////////////////////////////////////////////////////////////////
/// constructor
///
/// As argument, one specifies engine kind which should be
/// created like "http:8080". One could specify several engines
/// at once, separating them with semicolon (";"). Following engines are supported:
///
///       http - TCivetweb, civetweb-based implementation of http protocol
///       fastcgi - TFastCgi, special protocol for communicating with web servers
///
/// For each created engine one should provide socket port number like "http:8080" or "fastcgi:9000".
/// Additional engine-specific options can be supplied with URL syntax like "http:8080?thrds=10".
/// Full list of supported options should be checked in engines docu.
///
/// One also can configure following options, separated by semicolon:
///
///     readonly, ro   - set read-only mode (default)
///     readwrite, rw  - allows methods execution of registered objects
///     global         - scans global ROOT lists for existing objects (default)
///     noglobal       - disable scan of global lists
///     cors           - enable CORS header with origin="*"
///     cors=domain    - enable CORS header with origin="domain"
///     basic_sniffer  - use basic sniffer without support of hist, gpad, graph classes
///
/// For example, create http server, which allows cors headers and disable scan of global lists,
/// one should provide "http:8080;cors;noglobal" as parameter
///
/// THttpServer uses JavaScript ROOT (https://root.cern/js) to implement web clients UI.
/// Normally JSROOT sources are used from $ROOTSYS/etc/http directory,
/// but one could set JSROOTSYS shell variable to specify alternative location

THttpServer::THttpServer(const char *engine) : TNamed("http", "ROOT http server")
{
   const char *jsrootsys = gSystem->Getenv("JSROOTSYS");
   if (jsrootsys)
      fJSROOTSYS = jsrootsys;

   if (fJSROOTSYS.Length() == 0) {
      TString jsdir = TString::Format("%s/http", TROOT::GetEtcDir().Data());
      if (gSystem->ExpandPathName(jsdir)) {
         Warning("THttpServer", "problems resolving '%s', use JSROOTSYS to specify $ROOTSYS/etc/http location",
                 jsdir.Data());
         fJSROOTSYS = ".";
      } else {
         fJSROOTSYS = jsdir;
      }
   }

   AddLocation("currentdir/", ".");
   AddLocation("jsrootsys/", fJSROOTSYS);
   AddLocation("rootsys/", TROOT::GetRootSys());

   fDefaultPage = fJSROOTSYS + "/files/online.htm";
   fDrawPage = fJSROOTSYS + "/files/draw.htm";

   TRootSniffer *sniff = nullptr;
   if (strstr(engine, "basic_sniffer")) {
      sniff = new TRootSniffer("sniff");
   } else {
      sniff = (TRootSniffer *)gROOT->ProcessLineSync("new TRootSnifferFull(\"sniff\");");
   }

   SetSniffer(sniff);

   // start timer
   SetTimer(20, kTRUE);

   if (strchr(engine, ';') == 0) {
      CreateEngine(engine);
   } else {
      TObjArray *lst = TString(engine).Tokenize(";");

      for (Int_t n = 0; n <= lst->GetLast(); n++) {
         const char *opt = lst->At(n)->GetName();
         if ((strcmp(opt, "readonly") == 0) || (strcmp(opt, "ro") == 0)) {
            GetSniffer()->SetReadOnly(kTRUE);
         } else if ((strcmp(opt, "readwrite") == 0) || (strcmp(opt, "rw") == 0)) {
            GetSniffer()->SetReadOnly(kFALSE);
         } else if (strcmp(opt, "global") == 0) {
            GetSniffer()->SetScanGlobalDir(kTRUE);
         } else if (strcmp(opt, "noglobal") == 0) {
            GetSniffer()->SetScanGlobalDir(kFALSE);
         } else if (strncmp(opt, "cors=", 5) == 0) {
            SetCors(opt + 5);
         } else if (strcmp(opt, "cors") == 0) {
            SetCors("*");
         } else
            CreateEngine(opt);
      }

      delete lst;
   }
}

////////////////////////////////////////////////////////////////////////////////
/// destructor
/// delete all http engines and sniffer

THttpServer::~THttpServer()
{
   THttpCallArg *arg = nullptr;
   Bool_t owner = kFALSE;
   while (true) {
      // delete outside the locked mutex area
      if (owner && arg)
         delete arg;

      std::unique_lock<std::mutex> lk(fMutex);

      if (fCallArgs.GetSize() == 0)
         break;
      arg = static_cast<THttpCallArg *>(fCallArgs.First());
      const char *opt = fCallArgs.FirstLink()->GetAddOption();
      owner = opt && !strcmp(opt, "owner");
      fCallArgs.RemoveFirst();
   }

   if (fTerminated) {
      TIter iter(&fEngines);
      THttpEngine *engine = nullptr;
      while ((engine = (THttpEngine *)iter()) != nullptr)
         engine->Terminate();
   }

   fEngines.Delete();

   SetSniffer(nullptr);

   SetTimer(0);
}

////////////////////////////////////////////////////////////////////////////////
/// Set TRootSniffer to the server
/// Server takes ownership over sniffer

void THttpServer::SetSniffer(TRootSniffer *sniff)
{
   if (fSniffer)
      delete fSniffer;
   fSniffer = sniff;
}

////////////////////////////////////////////////////////////////////////////////
/// Set termination flag,
/// No any further requests will be processed, server only can be destroyed afterwards

void THttpServer::SetTerminate()
{
   fTerminated = kTRUE;
}

////////////////////////////////////////////////////////////////////////////////
/// returns read-only mode

Bool_t THttpServer::IsReadOnly() const
{
   return fSniffer ? fSniffer->IsReadOnly() : kTRUE;
}

////////////////////////////////////////////////////////////////////////////////
/// Set read-only mode for the server (default on)
/// In read-only server is not allowed to change any ROOT object, registered to the server
/// Server also cannot execute objects method via exe.json request

void THttpServer::SetReadOnly(Bool_t readonly)
{
   if (fSniffer)
      fSniffer->SetReadOnly(readonly);
}

////////////////////////////////////////////////////////////////////////////////
/// add files location, which could be used in the server
/// one could map some system folder to the server like AddLocation("mydir/","/home/user/specials");
/// Than files from this directory could be addressed via server like
/// http://localhost:8080/mydir/myfile.root

void THttpServer::AddLocation(const char *prefix, const char *path)
{
   if (!prefix || (*prefix == 0))
      return;

   if (!path)
      fLocations.erase(fLocations.find(prefix));
   else
      fLocations[prefix] = path;
}

////////////////////////////////////////////////////////////////////////////////
/// Set location of JSROOT to use with the server
/// One could specify address like:
///   https://root.cern.ch/js/3.3/
///   http://web-docs.gsi.de/~linev/js/3.3/
/// This allows to get new JSROOT features with old server,
/// reduce load on THttpServer instance, also startup time can be improved
/// When empty string specified (default), local copy of JSROOT is used (distributed with ROOT)

void THttpServer::SetJSROOT(const char *location)
{
   fJSROOT = location ? location : "";
}

////////////////////////////////////////////////////////////////////////////////
/// Set file name of HTML page, delivered by the server when
/// http address is opened in the browser.
/// By default, $ROOTSYS/etc/http/files/online.htm page is used
/// When empty filename is specified, default page will be used

void THttpServer::SetDefaultPage(const std::string &filename)
{
   if (!filename.empty())
      fDefaultPage = filename;
   else
      fDefaultPage = fJSROOTSYS + "/files/online.htm";

   // force to read page content next time again
   fDefaultPageCont.clear();
}

////////////////////////////////////////////////////////////////////////////////
/// Set file name of HTML page, delivered by the server when
/// objects drawing page is requested from the browser
/// By default, $ROOTSYS/etc/http/files/draw.htm page is used
/// When empty filename is specified, default page will be used

void THttpServer::SetDrawPage(const std::string &filename)
{
   if (!filename.empty())
      fDrawPage = filename;
   else
      fDrawPage = fJSROOTSYS + "/files/draw.htm";

   // force to read page content next time again
   fDrawPageCont.clear();
}

////////////////////////////////////////////////////////////////////////////////
/// factory method to create different http engines
/// At the moment two engine kinds are supported:
///   civetweb (default) and fastcgi
/// Examples:
///   "http:8080" or "civetweb:8080" or ":8080"  - creates civetweb web server with http port 8080
///   "fastcgi:9000" - creates fastcgi server with port 9000
/// One could apply additional parameters, using URL syntax:
///    "http:8080?thrds=10"

Bool_t THttpServer::CreateEngine(const char *engine)
{
   if (!engine)
      return kFALSE;

   const char *arg = strchr(engine, ':');
   if (!arg)
      return kFALSE;

   TString clname;
   if (arg != engine)
      clname.Append(engine, arg - engine);

   THttpEngine *eng = nullptr;

   if ((clname.Length() == 0) || (clname == "http") || (clname == "civetweb")) {
      eng = new TCivetweb(kFALSE);
   } else if (clname == "https") {
      eng = new TCivetweb(kTRUE);
   } else if (clname == "fastcgi") {
      eng = new TFastCgi();
   }

   if (!eng) {
      // ensure that required engine class exists before we try to create it
      TClass *engine_class = gROOT->LoadClass(clname.Data());
      if (!engine_class)
         return kFALSE;

      eng = (THttpEngine *)engine_class->New();
      if (!eng)
         return kFALSE;
   }

   eng->SetServer(this);

   if (!eng->Create(arg + 1)) {
      delete eng;
      return kFALSE;
   }

   fEngines.Add(eng);

   return kTRUE;
}

////////////////////////////////////////////////////////////////////////////////
/// create timer which will invoke ProcessRequests() function periodically
/// Timer is required to perform all actions in main ROOT thread
/// Method arguments are the same as for TTimer constructor
/// By default, sync timer with 100 ms period is created
///
/// It is recommended to always use sync timer mode and only change period to
/// adjust server reaction time. Use of async timer requires, that application regularly
/// calls gSystem->ProcessEvents(). It happens automatically in ROOT interactive shell.
/// If milliSec == 0, no timer will be created.
/// In this case application should regularly call ProcessRequests() method.
///
/// Async timer allows to use THttpServer in applications, which does not have explicit
/// gSystem->ProcessEvents() calls. But be aware, that such timer can interrupt any system call
/// (lise malloc) and can lead to dead locks, especially in multi-threaded applications.

void THttpServer::SetTimer(Long_t milliSec, Bool_t mode)
{
   if (fTimer) {
      fTimer->Stop();
      delete fTimer;
      fTimer = nullptr;
   }
   if (milliSec > 0) {
      fTimer = new THttpTimer(milliSec, mode, *this);
      fTimer->TurnOn();
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Checked that filename does not contains relative path below current directory
/// Used to prevent access to files below current directory

Bool_t THttpServer::VerifyFilePath(const char *fname)
{
   if (!fname || (*fname == 0))
      return kFALSE;

   Int_t level = 0;

   while (*fname != 0) {

      // find next slash or backslash
      const char *next = strpbrk(fname, "/\\");
      if (next == 0)
         return kTRUE;

      // most important - change to parent dir
      if ((next == fname + 2) && (*fname == '.') && (*(fname + 1) == '.')) {
         fname += 3;
         level--;
         if (level < 0)
            return kFALSE;
         continue;
      }

      // ignore current directory
      if ((next == fname + 1) && (*fname == '.')) {
         fname += 2;
         continue;
      }

      // ignore slash at the front
      if (next == fname) {
         fname++;
         continue;
      }

      fname = next + 1;
      level++;
   }

   return kTRUE;
}

////////////////////////////////////////////////////////////////////////////////
/// Verifies that request is just file name
/// File names typically contains prefix like "jsrootsys/"
/// If true, method returns real name of the file,
/// which should be delivered to the client
/// Method is thread safe and can be called from any thread

Bool_t THttpServer::IsFileRequested(const char *uri, TString &res) const
{
   if (!uri || (*uri == 0))
      return kFALSE;

   TString fname(uri);

   for (auto iter = fLocations.begin(); iter != fLocations.end(); iter++) {
      Ssiz_t pos = fname.Index(iter->first.c_str());
      if (pos == kNPOS)
         continue;
      fname.Remove(0, pos + (iter->first.length() - 1));
      if (!VerifyFilePath(fname.Data()))
         return kFALSE;
      res = iter->second.c_str();
      if ((fname[0] == '/') && (res[res.Length() - 1] == '/'))
         res.Resize(res.Length() - 1);
      res.Append(fname);
      return kTRUE;
   }

   return kFALSE;
}

////////////////////////////////////////////////////////////////////////////////
/// Executes http request, specified in THttpCallArg structure
/// Method can be called from any thread
/// Actual execution will be done in main ROOT thread, where analysis code is running.

Bool_t THttpServer::ExecuteHttp(std::shared_ptr<THttpCallArg> arg)
{
   if (fTerminated)
      return kFALSE;

   if ((fMainThrdId != 0) && (fMainThrdId == TThread::SelfId())) {
      // should not happen, but one could process requests directly without any signaling

      ProcessRequest(arg);

      return kTRUE;
   }

   // add call arg to the list
   std::unique_lock<std::mutex> lk(fMutex);
   fArgs.push(arg);
   // and now wait until request is processed
   arg->fCond.wait(lk);

   return kTRUE;
}

////////////////////////////////////////////////////////////////////////////////
/// \deprecated Signature with shared_ptr should be used
/// Executes http request, specified in THttpCallArg structure
/// Method can be called from any thread
/// Actual execution will be done in main ROOT thread, where analysis code is running.

Bool_t THttpServer::ExecuteHttp(THttpCallArg *arg)
{
   if (fTerminated)
      return kFALSE;

   if ((fMainThrdId != 0) && (fMainThrdId == TThread::SelfId())) {
      // should not happen, but one could process requests directly without any signaling

      ProcessRequest(arg);

      return kTRUE;
   }

   // add call arg to the list
   std::unique_lock<std::mutex> lk(fMutex);
   fCallArgs.Add(arg);
   // and now wait until request is processed
   arg->fCond.wait(lk);

   return kTRUE;
}

////////////////////////////////////////////////////////////////////////////////
/// \deprecated Signature with shared_ptr should be used
/// Submit http request, specified in THttpCallArg structure
/// Contrary to ExecuteHttp, it will not block calling thread.
/// User should reimplement THttpCallArg::HttpReplied() method
/// to react when HTTP request is executed.
/// Method can be called from any thread
/// Actual execution will be done in main ROOT thread, where analysis code is running.
/// When called from main thread and can_run_immediately==kTRUE, will be
/// executed immediately.
/// If ownership==kTRUE, THttpCallArg object will be destroyed by the THttpServer
/// Returns kTRUE when was executed.

Bool_t THttpServer::SubmitHttp(THttpCallArg *arg, Bool_t can_run_immediately, Bool_t ownership)
{
   if (fTerminated) {
      if (ownership)
         delete arg;
      return kFALSE;
   }

   if (can_run_immediately && (fMainThrdId != 0) && (fMainThrdId == TThread::SelfId())) {
      ProcessRequest(arg);
      arg->NotifyCondition();
      if (ownership)
         delete arg;
      return kTRUE;
   }

   // add call arg to the list
   std::unique_lock<std::mutex> lk(fMutex);
   if (ownership)
      fArgs.push(std::shared_ptr<THttpCallArg>(arg));
   else
      fCallArgs.Add(arg);
   return kFALSE;
}

////////////////////////////////////////////////////////////////////////////////
/// Submit http request, specified in THttpCallArg structure
/// Contrary to ExecuteHttp, it will not block calling thread.
/// User should reimplement THttpCallArg::HttpReplied() method
/// to react when HTTP request is executed.
/// Method can be called from any thread
/// Actual execution will be done in main ROOT thread, where analysis code is running.
/// When called from main thread and can_run_immediately==kTRUE, will be
/// executed immediately.
/// Returns kTRUE when was executed.

Bool_t THttpServer::SubmitHttp(std::shared_ptr<THttpCallArg> arg, Bool_t can_run_immediately)
{
   if (fTerminated)
      return kFALSE;

   if (can_run_immediately && (fMainThrdId != 0) && (fMainThrdId == TThread::SelfId())) {
      ProcessRequest(arg);
      arg->NotifyCondition();
      return kTRUE;
   }

   // add call arg to the list
   std::unique_lock<std::mutex> lk(fMutex);
   fArgs.push(arg);
   return kFALSE;
}

////////////////////////////////////////////////////////////////////////////////
/// Process requests, submitted for execution
/// Regularly invoked by THttpTimer, when somewhere in the code
/// gSystem->ProcessEvents() is called.
/// User can call serv->ProcessRequests() directly, but only from main analysis thread.

void THttpServer::ProcessRequests()
{
   if (fMainThrdId == 0)
      fMainThrdId = TThread::SelfId();

   if (fMainThrdId != TThread::SelfId()) {
      Error("ProcessRequests", "Should be called only from main ROOT thread");
      return;
   }

   std::unique_lock<std::mutex> lk(fMutex, std::defer_lock);

   // first process requests in the queue
   while (true) {
      std::shared_ptr<THttpCallArg> arg;

      lk.lock();
      if (!fArgs.empty()) {
         arg = fArgs.front();
         fArgs.pop();
      }
      lk.unlock();

      if (!arg)
         break;

      fSniffer->SetCurrentCallArg(arg.get());

      try {
         ProcessRequest(arg);
         fSniffer->SetCurrentCallArg(nullptr);
      } catch (...) {
         fSniffer->SetCurrentCallArg(nullptr);
      }

      arg->NotifyCondition();
   }

   // then process old-style queue, will be removed later
   while (true) {
      THttpCallArg *arg = nullptr;

      lk.lock();
      if (fCallArgs.GetSize() > 0) {
         arg = static_cast<THttpCallArg *>(fCallArgs.First());
         fCallArgs.RemoveFirst();
      }
      lk.unlock();

      if (!arg)
         break;

      fSniffer->SetCurrentCallArg(arg);

      try {
         ProcessRequest(arg);
         fSniffer->SetCurrentCallArg(nullptr);
      } catch (...) {
         fSniffer->SetCurrentCallArg(nullptr);
      }

      arg->NotifyCondition();
   }

   // regularly call Process() method of engine to let perform actions in ROOT context
   TIter iter(&fEngines);
   THttpEngine *engine = nullptr;
   while ((engine = (THttpEngine *)iter()) != nullptr) {
      if (fTerminated)
         engine->Terminate();
      engine->Process();
   }
}

////////////////////////////////////////////////////////////////////////////////
/// Method called when THttpServer cannot process request
/// By default such requests replied with 404 code
/// One could overwrite with method in derived class to process all kinds of such non-standard requests

void THttpServer::MissedRequest(THttpCallArg *arg)
{
   arg->Set404();
}

////////////////////////////////////////////////////////////////////////////////
/// Process single http request
/// Depending from requested path and filename different actions will be performed.
/// In most cases information is provided by TRootSniffer class

void THttpServer::ProcessRequest(std::shared_ptr<THttpCallArg> arg)
{
   if (fTerminated) {
      arg->Set404();
      return;
   }

   if ((arg->fFileName != "root.websocket") && (arg->fFileName != "root.longpoll"))
      return ProcessRequest(arg.get());

   THttpWSHandler *handler = dynamic_cast<THttpWSHandler *>(fSniffer->FindTObjectInHierarchy(arg->fPathName.Data()));

   if (!handler) {
      arg->Set404();
      return;
   }

   if (arg->fFileName == "root.websocket") {
      // handling of web socket
      if (!handler->HandleWS(arg))
         arg->Set404();
   } else if (arg->fFileName == "root.longpoll") {
      // ROOT emulation of websocket with polling requests
      if ((arg->fQuery == "connect") || (arg->fQuery == "connect_raw")) {
         // try to emulate websocket connect
         // if accepted, reply with connection id, which must be used in the following communications
         arg->SetMethod("WS_CONNECT");

         bool israw = (arg->fQuery == "connect_raw");

         // automatically assign engine to arg
         arg->CreateWSEngine<THttpLongPollEngine>(israw);

         if (handler->HandleWS(arg)) {
            arg->SetMethod("WS_READY");

            if (handler->HandleWS(arg))
               arg->SetTextContent(std::string(israw ? "txt:" : "") + std::to_string(arg->GetWSId()));
         } else {
            arg->TakeWSEngine(); // delete handle
         }
         if (!arg->IsText())
            arg->Set404();
      } else {
         TUrl url;
         url.SetOptions(arg->fQuery);
         url.ParseOptions();
         Int_t connid = url.GetIntValueFromOptions("connection");
         arg->SetWSId((UInt_t)connid);
         if (url.HasOption("close")) {
            arg->SetMethod("WS_CLOSE");
            arg->SetTextContent("OK");
         } else {
            arg->SetMethod("WS_DATA");
         }

         if (!handler->HandleWS(arg))
            arg->Set404();
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
/// \deprecated  One should use signature with std::shared_ptr
/// Process single http request
/// Depending from requested path and filename different actions will be performed.
/// In most cases information is provided by TRootSniffer class

void THttpServer::ProcessRequest(THttpCallArg *arg)
{
   if (fTerminated) {
      arg->Set404();
      return;
   }

   if (arg->fFileName.IsNull() || (arg->fFileName == "index.htm")) {

      THttpWSHandler *handler(nullptr);

      if (arg->fFileName.IsNull())
         handler = dynamic_cast<THttpWSHandler *>(fSniffer->FindTObjectInHierarchy(arg->fPathName.Data()));

      if (handler) {

         arg->fContent = handler->GetDefaultPageContent().Data();

         if (arg->fContent.find("file:") == 0) {
            TString fname = arg->fContent.c_str() + 5;
            fname.ReplaceAll("$jsrootsys", fJSROOTSYS);

            arg->fContent = ReadFileContent(fname.Data());
         }
      }

      if (arg->fContent.empty()) {

         if (fDefaultPageCont.empty())
            fDefaultPageCont = ReadFileContent(fDefaultPage);

         arg->fContent = fDefaultPageCont;
      }

      if (arg->fContent.empty()) {
         arg->Set404();
      } else {
         // replace all references on JSROOT
         if (fJSROOT.Length() > 0) {
            std::string repl("=\"");
            repl.append(fJSROOT.Data());
            if (repl.back() != '/')
               repl.append("/");
            arg->ReplaceAllinContent("=\"jsrootsys/", repl);
         }

         const char *hjsontag = "\"$$$h.json$$$\"";

         // add h.json caching
         if (arg->fContent.find(hjsontag) != std::string::npos) {
            TString h_json;
            TRootSnifferStoreJson store(h_json, kTRUE);
            const char *topname = fTopName.Data();
            if (arg->fTopName.Length() > 0)
               topname = arg->fTopName.Data();
            fSniffer->ScanHierarchy(topname, arg->fPathName.Data(), &store);

            arg->ReplaceAllinContent(hjsontag, h_json.Data());

            arg->AddHeader("Cache-Control",
                           "private, no-cache, no-store, must-revalidate, max-age=0, proxy-revalidate, s-maxage=0");
            if (arg->fQuery.Index("nozip") == kNPOS)
               arg->SetZipping();
         }
         arg->SetContentType("text/html");
      }
      return;
   }

   if (arg->fFileName == "draw.htm") {
      if (fDrawPageCont.empty())
         fDrawPageCont = ReadFileContent(fDrawPage);

      if (fDrawPageCont.empty()) {
         arg->Set404();
      } else {
         const char *rootjsontag = "\"$$$root.json$$$\"";
         const char *hjsontag = "\"$$$h.json$$$\"";

         arg->fContent = fDrawPageCont;

         // replace all references on JSROOT
         if (fJSROOT.Length() > 0) {
            std::string repl("=\"");
            repl.append(fJSROOT.Data());
            if (repl.back() != '/')
               repl.append("/");
            arg->ReplaceAllinContent("=\"jsrootsys/", repl);
         }

         if ((arg->fQuery.Index("no_h_json") == kNPOS) && (arg->fQuery.Index("webcanvas") == kNPOS) &&
             (arg->fContent.find(hjsontag) != std::string::npos)) {
            TString h_json;
            TRootSnifferStoreJson store(h_json, kTRUE);
            const char *topname = fTopName.Data();
            if (arg->fTopName.Length() > 0)
               topname = arg->fTopName.Data();
            fSniffer->ScanHierarchy(topname, arg->fPathName.Data(), &store, kTRUE);

            arg->ReplaceAllinContent(hjsontag, h_json.Data());
         }

         if ((arg->fQuery.Index("no_root_json") == kNPOS) && (arg->fQuery.Index("webcanvas") == kNPOS) &&
             (arg->fContent.find(rootjsontag) != std::string::npos)) {
            std::string str;
            if (fSniffer->Produce(arg->fPathName.Data(), "root.json", "compact=23", str))
               arg->ReplaceAllinContent(rootjsontag, str);
         }
         arg->AddHeader("Cache-Control",
                        "private, no-cache, no-store, must-revalidate, max-age=0, proxy-revalidate, s-maxage=0");
         if (arg->fQuery.Index("nozip") == kNPOS)
            arg->SetZipping();
         arg->SetContentType("text/html");
      }
      return;
   }

   if ((arg->fFileName == "favicon.ico") && arg->fPathName.IsNull()) {
      arg->SetFile(fJSROOTSYS + "/img/RootIcon.ico");
      return;
   }

   TString filename;
   if (IsFileRequested(arg->fFileName.Data(), filename)) {
      arg->SetFile(filename);
      return;
   }

   filename = arg->fFileName;
   Bool_t iszip = kFALSE;
   if (filename.EndsWith(".gz")) {
      filename.Resize(filename.Length() - 3);
      iszip = kTRUE;
   }

   if ((filename == "h.xml") || (filename == "get.xml")) {

      Bool_t compact = arg->fQuery.Index("compact") != kNPOS;

      TString res;

      res.Form("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
      if (!compact)
         res.Append("\n");
      res.Append("<root>");
      if (!compact)
         res.Append("\n");
      {
         TRootSnifferStoreXml store(res, compact);

         const char *topname = fTopName.Data();
         if (arg->fTopName.Length() > 0)
            topname = arg->fTopName.Data();
         fSniffer->ScanHierarchy(topname, arg->fPathName.Data(), &store, filename == "get.xml");
      }

      res.Append("</root>");
      if (!compact)
         res.Append("\n");

      arg->SetContent(std::string(res.Data()));

      arg->SetXml();
   } else if (filename == "h.json") {
      TString res;
      TRootSnifferStoreJson store(res, arg->fQuery.Index("compact") != kNPOS);
      const char *topname = fTopName.Data();
      if (arg->fTopName.Length() > 0)
         topname = arg->fTopName.Data();
      fSniffer->ScanHierarchy(topname, arg->fPathName.Data(), &store);
      arg->SetContent(std::string(res.Data()));
      arg->SetJson();
   } else if (fSniffer->Produce(arg->fPathName.Data(), filename.Data(), arg->fQuery.Data(), arg->fContent)) {
      // define content type base on extension
      arg->SetContentType(GetMimeType(filename.Data()));
   } else {
      // miss request, user may process
      MissedRequest(arg);
   }

   if (arg->Is404())
      return;

   if (iszip)
      arg->SetZipping(THttpCallArg::kZipAlways);

   if (filename == "root.bin") {
      // only for binary data master version is important
      // it allows to detect if streamer info was modified
      const char *parname = fSniffer->IsStreamerInfoItem(arg->fPathName.Data()) ? "BVersion" : "MVersion";
      arg->AddHeader(parname, Form("%u", (unsigned)fSniffer->GetStreamerInfoHash()));
   }

   // try to avoid caching on the browser
   arg->AddHeader("Cache-Control",
                  "private, no-cache, no-store, must-revalidate, max-age=0, proxy-revalidate, s-maxage=0");

   // potentially add cors header
   if (IsCors())
      arg->AddHeader("Access-Control-Allow-Origin", GetCors());
}

////////////////////////////////////////////////////////////////////////////////
/// Register object in folders hierarchy
///
/// See TRootSniffer::RegisterObject() for more details

Bool_t THttpServer::Register(const char *subfolder, TObject *obj)
{
   return fSniffer->RegisterObject(subfolder, obj);
}

////////////////////////////////////////////////////////////////////////////////
/// Unregister object in folders hierarchy
///
/// See TRootSniffer::UnregisterObject() for more details

Bool_t THttpServer::Unregister(TObject *obj)
{
   return fSniffer->UnregisterObject(obj);
}

////////////////////////////////////////////////////////////////////////////////
/// Restrict access to specified object
///
/// See TRootSniffer::Restrict() for more details

void THttpServer::Restrict(const char *path, const char *options)
{
   fSniffer->Restrict(path, options);
}

////////////////////////////////////////////////////////////////////////////////
/// Register command which can be executed from web interface
///
/// As method one typically specifies string, which is executed with
/// gROOT->ProcessLine() method. For instance
///    serv->RegisterCommand("Invoke","InvokeFunction()");
///
/// Or one could specify any method of the object which is already registered
/// to the server. For instance:
///     serv->Register("/", hpx);
///     serv->RegisterCommand("/ResetHPX", "/hpx/->Reset()");
/// Here symbols '/->' separates item name from method to be executed
///
/// One could specify additional arguments in the command with
/// syntax like %arg1%, %arg2% and so on. For example:
///     serv->RegisterCommand("/ResetHPX", "/hpx/->SetTitle(\"%arg1%\")");
///     serv->RegisterCommand("/RebinHPXPY", "/hpxpy/->Rebin2D(%arg1%,%arg2%)");
/// Such parameter(s) will be requested when command clicked in the browser.
///
/// Once command is registered, one could specify icon which will appear in the browser:
///     serv->SetIcon("/ResetHPX", "rootsys/icons/ed_execute.png");
///
/// One also can set extra property '_fastcmd', that command appear as
/// tool button on the top of the browser tree:
///     serv->SetItemField("/ResetHPX", "_fastcmd", "true");
/// Or it is equivalent to specifying extra argument when register command:
///     serv->RegisterCommand("/ResetHPX", "/hpx/->Reset()", "button;rootsys/icons/ed_delete.png");

Bool_t THttpServer::RegisterCommand(const char *cmdname, const char *method, const char *icon)
{
   return fSniffer->RegisterCommand(cmdname, method, icon);
}

////////////////////////////////////////////////////////////////////////////////
/// hides folder or element from web gui

Bool_t THttpServer::Hide(const char *foldername, Bool_t hide)
{
   return SetItemField(foldername, "_hidden", hide ? "true" : (const char *)0);
}

////////////////////////////////////////////////////////////////////////////////
/// set name of icon, used in browser together with the item
///
/// One could use images from $ROOTSYS directory like:
///    serv->SetIcon("/ResetHPX","/rootsys/icons/ed_execute.png");

Bool_t THttpServer::SetIcon(const char *fullname, const char *iconname)
{
   return SetItemField(fullname, "_icon", iconname);
}

////////////////////////////////////////////////////////////////////////////////

Bool_t THttpServer::CreateItem(const char *fullname, const char *title)
{
   return fSniffer->CreateItem(fullname, title);
}

////////////////////////////////////////////////////////////////////////////////

Bool_t THttpServer::SetItemField(const char *fullname, const char *name, const char *value)
{
   return fSniffer->SetItemField(fullname, name, value);
}

////////////////////////////////////////////////////////////////////////////////

const char *THttpServer::GetItemField(const char *fullname, const char *name)
{
   return fSniffer->GetItemField(fullname, name);
}

////////////////////////////////////////////////////////////////////////////////
/// Returns MIME type base on file extension

const char *THttpServer::GetMimeType(const char *path)
{
   static const struct {
      const char *extension;
      int ext_len;
      const char *mime_type;
   } builtin_mime_types[] = {{".xml", 4, "text/xml"},
                             {".json", 5, "application/json"},
                             {".bin", 4, "application/x-binary"},
                             {".gif", 4, "image/gif"},
                             {".jpg", 4, "image/jpeg"},
                             {".png", 4, "image/png"},
                             {".html", 5, "text/html"},
                             {".htm", 4, "text/html"},
                             {".shtm", 5, "text/html"},
                             {".shtml", 6, "text/html"},
                             {".css", 4, "text/css"},
                             {".js", 3, "application/x-javascript"},
                             {".ico", 4, "image/x-icon"},
                             {".jpeg", 5, "image/jpeg"},
                             {".svg", 4, "image/svg+xml"},
                             {".txt", 4, "text/plain"},
                             {".torrent", 8, "application/x-bittorrent"},
                             {".wav", 4, "audio/x-wav"},
                             {".mp3", 4, "audio/x-mp3"},
                             {".mid", 4, "audio/mid"},
                             {".m3u", 4, "audio/x-mpegurl"},
                             {".ogg", 4, "application/ogg"},
                             {".ram", 4, "audio/x-pn-realaudio"},
                             {".xslt", 5, "application/xml"},
                             {".xsl", 4, "application/xml"},
                             {".ra", 3, "audio/x-pn-realaudio"},
                             {".doc", 4, "application/msword"},
                             {".exe", 4, "application/octet-stream"},
                             {".zip", 4, "application/x-zip-compressed"},
                             {".xls", 4, "application/excel"},
                             {".tgz", 4, "application/x-tar-gz"},
                             {".tar", 4, "application/x-tar"},
                             {".gz", 3, "application/x-gunzip"},
                             {".arj", 4, "application/x-arj-compressed"},
                             {".rar", 4, "application/x-arj-compressed"},
                             {".rtf", 4, "application/rtf"},
                             {".pdf", 4, "application/pdf"},
                             {".swf", 4, "application/x-shockwave-flash"},
                             {".mpg", 4, "video/mpeg"},
                             {".webm", 5, "video/webm"},
                             {".mpeg", 5, "video/mpeg"},
                             {".mov", 4, "video/quicktime"},
                             {".mp4", 4, "video/mp4"},
                             {".m4v", 4, "video/x-m4v"},
                             {".asf", 4, "video/x-ms-asf"},
                             {".avi", 4, "video/x-msvideo"},
                             {".bmp", 4, "image/bmp"},
                             {".ttf", 4, "application/x-font-ttf"},
                             {NULL, 0, NULL}};

   int path_len = strlen(path);

   for (int i = 0; builtin_mime_types[i].extension != NULL; i++) {
      if (path_len <= builtin_mime_types[i].ext_len)
         continue;
      const char *ext = path + (path_len - builtin_mime_types[i].ext_len);
      if (strcmp(ext, builtin_mime_types[i].extension) == 0) {
         return builtin_mime_types[i].mime_type;
      }
   }

   return "text/plain";
}

////////////////////////////////////////////////////////////////////////////////
/// reads file content

char *THttpServer::ReadFileContent(const char *filename, Int_t &len)
{
   len = 0;

   std::ifstream is(filename);
   if (!is)
      return 0;

   is.seekg(0, is.end);
   len = is.tellg();
   is.seekg(0, is.beg);

   char *buf = (char *)malloc(len);
   is.read(buf, len);
   if (!is) {
      free(buf);
      len = 0;
      return 0;
   }

   return buf;
}

////////////////////////////////////////////////////////////////////////////////
/// reads file content, using std::string as container

std::string THttpServer::ReadFileContent(const std::string &filename)
{
   std::ifstream is(filename);
   std::string res;
   if (is) {
      is.seekg(0, std::ios::end);
      res.resize(is.tellg());
      is.seekg(0, std::ios::beg);
      is.read((char *)res.data(), res.length());
      if (!is)
         res.clear();
   }
   return res;
}
