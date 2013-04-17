// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/webdriver/webdriver_session.h"

#include <sstream>
#include <vector>

#include "base/bind.h"
#include "base/callback.h"
//#include "base/command_line.h"
#include "base/file_path.h"
#include "base/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop_proxy.h"
//#include "base/process.h"
//#include "base/process_util.h"
#include "base/string_number_conversions.h"
#include "base/string_split.h"
#include "base/string_util.h"
#include "base/stringprintf.h"
#include "base/synchronization/waitable_event.h"
//#include "base/test/test_timeouts.h"
#include "base/threading/platform_thread.h"
#include "base/time.h"
#include "base/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/automation/automation_json_requests.h"
#include "chrome/test/automation/value_conversion_util.h"
#include "chrome/test/webdriver/webdriver_capabilities_parser.h"
#include "chrome/test/webdriver/webdriver_error.h"
#include "chrome/test/webdriver/webdriver_key_converter.h"
#include "chrome/test/webdriver/webdriver_logging.h"
#include "chrome/test/webdriver/webdriver_session_manager.h"
#include "chrome/test/webdriver/webdriver_util.h"
#include "third_party/webdriver/atoms.h"

#include <QtCore/QDebug>
#if (QT_VERSION >= QT_VERSION_CHECK(5, 0, 0))
#include <QtWidgets/QApplication>
#else
#include <QtGui/QApplication>
#endif

using automation::kLeftButton;
using automation::kMouseDown;
using automation::kMouseMove;
using automation::kMouseUp;
using automation::kNoButton;

namespace webdriver {

namespace {
// This is the minimum version of chrome that supports the new mouse API.
const int kNewMouseAPIMinVersion = 1002;
}



FrameId::FrameId() {}

FrameId::FrameId(const WebViewId& view_id, const FramePath& frame_path)
    : view_id(view_id),
      frame_path(frame_path) {
}

Session::Session()
    : session_log_(new InMemoryLog()),
      logger_(kAllLogLevel),
      id_(GenerateRandomID()),
      current_target_(FrameId(WebViewId(), FramePath())),
      thread_(id_.c_str()),
      async_script_timeout_(0),
      implicit_wait_(0),
      has_alert_prompt_text_(false),
      sticky_modifiers_(0),
      build_no_(0) {
  SessionManager::GetInstance()->Add(this);
  logger_.AddHandler(session_log_.get());
  if (FileLog::Get())
    logger_.AddHandler(FileLog::Get());
}

Session::~Session() {
  SessionManager::GetInstance()->Remove(id_);
}

Error* Session::Init(const DictionaryValue* capabilities_dict) {

  if (!thread_.Start()) {
    delete this;
    return new Error(kUnknownError, "Cannot start session thread");
  }
  qtask.moveToThread(QApplication::instance()->thread());
  if (!temp_dir_.CreateUniqueTempDir()) {
    delete this;
    return new Error(
        kUnknownError, "Unable to create temp directory for unpacking");
  }
  logger_.Log(kFineLogLevel,
              "Initializing session with capabilities " +
                  JsonStringifyForDisplay(capabilities_dict));
  CapabilitiesParser parser(
      capabilities_dict, temp_dir_.path(), logger_, &capabilities_);
  Error* error = parser.Parse();
  if (error) {
    delete this;
    return error;
  }
  logger_.set_min_log_level(capabilities_.log_levels[LogType::kDriver]);

  Automation::BrowserOptions browser_options;
  //browser_options.command = capabilities_.command;
  browser_options.channel_id = capabilities_.channel;
  browser_options.browser_start_window = capabilities_.browser_start_window;
  browser_options.detach_process = capabilities_.detach;
  browser_options.user_data_dir = capabilities_.profile;
  browser_options.command = capabilities_.command;
  browser_options.browser_class = capabilities_.browser_class;
  if (!capabilities_.no_website_testing_defaults) {
    browser_options.ignore_certificate_errors = true;
  }

  RunSessionTask(base::Bind(
      &Session::InitOnSessionThread,
      base::Unretained(this),
      browser_options,
      &build_no_,
      &error));
  if (!error)
    error = PostBrowserStartInit();

  if (error)
    Terminate();
  return error;
}

Error* Session::BeforeExecuteCommand() {
  Error* error = AfterExecuteCommand();
  if (!error) {
    scoped_ptr<Error> switch_error(SwitchToTopFrameIfCurrentFrameInvalid());
    if (switch_error.get()) {
      std::string text;
      scoped_ptr<Error> alert_error(GetAlertMessage(&text));
      if (alert_error.get()) {
        // Only return a frame checking error if a modal dialog is not present.
        // TODO(kkania): This is ugly. Fix.
        return switch_error.release();
      }
    }
  }
  return error;
}

Error* Session::AfterExecuteCommand() {
  Error* error = NULL;
  if (!capabilities_.load_async) {
    error = WaitForAllViewsToStopLoading();
  }
  return error;
}

void Session::Terminate() {
  RunSessionTask(base::Bind(
      &Session::TerminateOnSessionThread,
      base::Unretained(this)));
  delete this;
}

Error* Session::ExecuteScript(const FrameId& frame_id,
                              const std::string& script,
                              const ListValue* const args,
                              Value** value) {
    if (!frame_id.view_id.IsTab()) {
      return new Error(kUnknownError,
                       "The current target does not support script execution");
    }

  std::string args_as_json;
  base::JSONWriter::Write(static_cast<const Value* const>(args),
                          &args_as_json);

  // Every injected script is fed through the executeScript atom. This atom
  // will catch any errors that are thrown and convert them to the
  // appropriate JSON structure.
  std::string jscript = base::StringPrintf(
      "function runScript() {return %s.apply(null,"
      "[function(){%s\n},%s,true])}; runScript()",
      atoms::asString(atoms::EXECUTE_SCRIPT).c_str(), script.c_str(),
      args_as_json.c_str());



  return ExecuteScriptAndParseValue(frame_id, jscript, value, false);
}

Error* Session::ExecuteScript(const std::string& script,
                              const ListValue* const args,
                              Value** value) {
  return ExecuteScript(current_target_, script, args, value);
}

Error* Session::ExecuteScriptAndParse(const FrameId& frame_id,
                                      const std::string& anonymous_func_script,
                                      const std::string& script_name,
                                      const ListValue* args,
                                      const ValueParser* parser) {
  scoped_ptr<const ListValue> scoped_args(args);
  scoped_ptr<const ValueParser> scoped_parser(parser);
  std::string called_script = base::StringPrintf(
      "return (%s).apply(null, arguments);", anonymous_func_script.c_str());
  Value* unscoped_value = NULL;
  Error* error = ExecuteScript(frame_id, called_script, args, &unscoped_value);
  if (error) {
    error->AddDetails(script_name + " execution failed");
    return error;
  }

  scoped_ptr<Value> value(unscoped_value);
  std::string error_msg;
  if (!parser->Parse(value.get())) {
    error_msg = base::StringPrintf("%s returned invalid value: %s",
        script_name.c_str(), JsonStringify(value.get()).c_str());
    return new Error(kUnknownError, error_msg);
  }
  return NULL;
}

Error* Session::ExecuteAsyncScript(const FrameId& frame_id,
                                   const std::string& script,
                                   const ListValue* const args,
                                   Value** value) {

    if (!frame_id.view_id.IsTab()) {
      return new Error(kUnknownError,
                       "The current target does not support script execution");
    }

  std::string args_as_json;
  base::JSONWriter::Write(static_cast<const Value* const>(args),
                          &args_as_json);

  int timeout_ms = async_script_timeout();

  // Every injected script is fed through the executeScript atom. This atom
  // will catch any errors that are thrown and convert them to the
  // appropriate JSON structure.
  std::string jscript = base::StringPrintf(
      "(%s).apply(null, [function(){%s},%s,%d,%s,true]);",
      atoms::asString(atoms::EXECUTE_ASYNC_SCRIPT).c_str(),
      script.c_str(),
      args_as_json.c_str(),
      timeout_ms,
      "function(result) {jsnotify.setResult(result);}");

  return ExecuteScriptAndParseValue(frame_id, jscript, value, true);
}

Error* Session::SendKeys(const ElementId& element, const string16& keys) {
    if (current_target_.view_id.IsApp())
    {
        Error* error = NULL;

        RunSessionTask(base::Bind(
                          &Session::SendKeysOnElementSessionThread,
                          base::Unretained(this),
                          element,
                          keys,
                          true /* release_modifiers */,
                          &error));
        return error;
    }

  bool is_displayed = false;
  Error* error = IsElementDisplayed(
      current_target_, element, true /* ignore_opacity */, &is_displayed);
  if (error)
    return error;
  if (!is_displayed)
    return new Error(kElementNotVisible);

  bool is_enabled = false;
  error = IsElementEnabled(current_target_, element, &is_enabled);
  if (error)
    return error;
  if (!is_enabled)
    return new Error(kInvalidElementState);

  // Focus the target element in order to send keys to it.
  // First, the currently active element is blurred, if it is different from
  // the target element. We do not want to blur an element unnecessarily,
  // because this may cause us to lose the current cursor position in the
  // element.
  // Secondly, we focus the target element.
  // Thirdly, if the target element is newly focused and is a text input, we
  // set the cursor position at the end.
  // Fourthly, we check if the new active element is the target element. If not,
  // we throw an error.
  // Additional notes:
  //   - |document.activeElement| is the currently focused element, or body if
  //     no element is focused
  //   - Even if |document.hasFocus()| returns true and the active element is
  //     the body, sometimes we still need to focus the body element for send
  //     keys to work. Not sure why
  //   - You cannot focus a descendant of a content editable node
  // TODO(jleyba): Update this to use the correct atom.
  const char* kFocusScript =
      "function(elem) {"
      "  var doc = elem.ownerDocument || elem;"
      "  var prevActiveElem = doc.activeElement;"
      "  if (elem != prevActiveElem && prevActiveElem)"
      "    prevActiveElem.blur();"
      "  elem.focus();"
      "  if (elem != prevActiveElem && elem.value && elem.value.length &&"
      "      elem.setSelectionRange) {"
      "    elem.setSelectionRange(elem.value.length, elem.value.length);"
      "  }"
      "  if (elem != doc.activeElement)"
      "    throw new Error('Failed to send keys because cannot focus element');"
      "}";
  error = ExecuteScriptAndParse(current_target_,
                                kFocusScript,
                                "focusElement",
                                CreateListValueFrom(element),
                                CreateDirectValueParser(kSkipParsing));
  if (error)
    return error;

  RunSessionTask(base::Bind(
      &Session::SendKeysOnSessionThread,
      base::Unretained(this),
      keys,
      true /* release_modifiers */,
      &error));
  return error;
}

Error* Session::SendKeys(const string16& keys) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Session::SendKeysOnSessionThread,
      base::Unretained(this),
      keys,
      false /* release_modifiers */,
      &error));
  return error;
}

Error* Session::DragAndDropFilePaths(
    const Point& location,
    const std::vector<FilePath::StringType>& paths) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::DragAndDropFilePaths,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      location,
      paths,
      &error));
  return error;
}

Error* Session::NavigateToURL(const std::string& url) {
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support navigation");
  }
  Error* error = NULL;
  if (capabilities_.load_async) {
    RunSessionTask(base::Bind(
        &Automation::NavigateToURLAsync,
        base::Unretained(automation_.get()),
        current_target_.view_id,
        url,
        &error));
  } else {
    RunSessionTask(base::Bind(
        &Automation::NavigateToURL,
        base::Unretained(automation_.get()),
        current_target_.view_id,
        url,
        &error));
  }
  return error;
}

Error* Session::GoForward() {
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support navigation");
  }
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::GoForward,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      &error));
  return error;
}

Error* Session::GoBack() {
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support navigation");
  }
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::GoBack,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      &error));
  return error;
}

Error* Session::Reload() {
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support navigation");
  }
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::Reload,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      &error));
  return error;
}

Error* Session::GetURL(std::string* url) {
    if (!current_target_.view_id.IsTab()) {
      return new Error(kUnknownError,
                       "The current target does not support URL source");
    }
    return ExecuteScriptAndParse(current_target_,
                               "function() { return document.URL }",
                               "getUrl",
                               new ListValue(),
                               CreateDirectValueParser(url));
}

Error* Session::GetTitle(std::string* tab_title) {
    if (current_target_.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetViewTitle,
            base::Unretained(automation_.get()),
            current_target_.view_id,
            tab_title,
            &error));

        return error;
    }

  const char* kGetTitleScript =
      "function() {"
      "  if (document.title)"
      "    return document.title;"
      "  else"
      "    return document.URL;"
      "}";
  return ExecuteScriptAndParse(FrameId(current_target_.view_id, FramePath()),
                               kGetTitleScript,
                               "getTitle",
                               new ListValue(),
                               CreateDirectValueParser(tab_title));
}

Error* Session::MouseMoveAndClick(const Point& location,
                                  automation::MouseButton button) {
  Error* error = NULL;
  // TODO : remove commented code, if don't need it
//  if (build_no_ >= kNewMouseAPIMinVersion) {
//    std::vector<WebMouseEvent> events;
//    events.push_back(CreateWebMouseEvent(kMouseMove, kNoButton, location, 0));
//    events.push_back(CreateWebMouseEvent(kMouseDown, button, location, 1));
//    events.push_back(CreateWebMouseEvent(kMouseUp, button, location, 1));
//    error = ProcessWebMouseEvents(events);
//  } else {
//    RunSessionTask(base::Bind(
//        &Automation::MouseClickDeprecated,
//        base::Unretained(automation_.get()),
//        current_target_.view_id,
//        location,
//        button,
//        &error));
//  }
  RunSessionTask(base::Bind(
          &Automation::MouseClickDeprecated,
          base::Unretained(automation_.get()),
          current_target_.view_id,
          location,
          button,
          &error));
  if (!error)
    mouse_position_ = location;
  return error;
}

Error* Session::MouseMove(const Point& location) {
  Error* error = NULL;
  // TODO : delete commented if don't need
//  if (build_no_ >= kNewMouseAPIMinVersion) {
//    std::vector<WebMouseEvent> events;
//    events.push_back(CreateWebMouseEvent(kMouseMove, kNoButton, location, 0));
//    error = ProcessWebMouseEvents(events);
//  } else {
//    RunSessionTask(base::Bind(
//        &Automation::MouseMoveDeprecated,
//        base::Unretained(automation_.get()),
//        current_target_.view_id,
//        location,
//        &error));
//  }
  RunSessionTask(base::Bind(
      &Automation::MouseMoveDeprecated,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      location,
      &error));
  if (!error)
    mouse_position_ = location;
  return error;
}

Error* Session::MouseDrag(const Point& start,
                          const Point& end)
{
  Error* error = NULL;
    // TODO : delete commented if don't need
//  if (build_no_ >= kNewMouseAPIMinVersion) {
//    std::vector<WebMouseEvent> events;
//    events.push_back(CreateWebMouseEvent(kMouseMove, kNoButton, start, 0));
//    events.push_back(CreateWebMouseEvent(kMouseDown, kLeftButton, start, 1));
//    events.push_back(CreateWebMouseEvent(kMouseMove, kLeftButton, end, 0));
//    events.push_back(CreateWebMouseEvent(kMouseUp, kLeftButton, end, 1));
//    error = ProcessWebMouseEvents(events);
//  } else {
//    RunSessionTask(base::Bind(
//        &Automation::MouseDragDeprecated,
//        base::Unretained(automation_.get()),
//        current_target_.view_id,
//        start,
//        end,
//        &error));
//  }
    RunSessionTask(base::Bind(
            &Automation::MouseDragDeprecated,
            base::Unretained(automation_.get()),
            current_target_.view_id,
            start,
            end,
            &error));
  if (!error)
    mouse_position_ = end;
  return error;
}

Error* Session::MouseClick(automation::MouseButton button)
{
    // TODO : delete commented if don't need
//  if (build_no_ >= kNewMouseAPIMinVersion) {
//    std::vector<WebMouseEvent> events;
//    events.push_back(CreateWebMouseEvent(
//        kMouseDown, button, mouse_position_, 1));
//    events.push_back(CreateWebMouseEvent(
//        kMouseUp, button, mouse_position_, 1));
//    return ProcessWebMouseEvents(events);
//  } else {
//    return MouseMoveAndClick(mouse_position_, button);
//  }
    return MouseMoveAndClick(mouse_position_, button);
}

Error* Session::MouseButtonDown() {
  Error* error = NULL;
  // TODO : delete commented if don't need
//  if (build_no_ >= kNewMouseAPIMinVersion) {
//    std::vector<WebMouseEvent> events;
//    events.push_back(CreateWebMouseEvent(
//        kMouseDown, kLeftButton, mouse_position_, 1));
//    error = ProcessWebMouseEvents(events);
//  } else {
//    RunSessionTask(base::Bind(
//        &Automation::MouseButtonDownDeprecated,
//        base::Unretained(automation_.get()),
//        current_target_.view_id,
//        mouse_position_,
//        &error));
//  }
  RunSessionTask(base::Bind(
          &Automation::MouseButtonDownDeprecated,
          base::Unretained(automation_.get()),
          current_target_.view_id,
          mouse_position_,
          &error));
  return error;
}

Error* Session::MouseButtonUp() {
  Error* error = NULL;
  // TODO : delete commented if don't need
//  if (build_no_ >= kNewMouseAPIMinVersion) {
//    std::vector<WebMouseEvent> events;
//    events.push_back(CreateWebMouseEvent(
//        kMouseUp, kLeftButton, mouse_position_, 1));
//    error = ProcessWebMouseEvents(events);
//  } else {
//    RunSessionTask(base::Bind(
//        &Automation::MouseButtonUpDeprecated,
//        base::Unretained(automation_.get()),
//        current_target_.view_id,
//        mouse_position_,
//        &error));
//  }
  RunSessionTask(base::Bind(
          &Automation::MouseButtonUpDeprecated,
          base::Unretained(automation_.get()),
          current_target_.view_id,
          mouse_position_,
          &error));
  return error;
}

Error* Session::MouseDoubleClick() {
  Error* error = NULL;
  // TODO : delete commented if don't need
//  if (build_no_ >= kNewMouseAPIMinVersion) {
//    std::vector<WebMouseEvent> events;
//    events.push_back(CreateWebMouseEvent(
//        kMouseDown, kLeftButton, mouse_position_, 1));
//    events.push_back(CreateWebMouseEvent(
//        kMouseUp, kLeftButton, mouse_position_, 1));
//    events.push_back(CreateWebMouseEvent(
//        kMouseDown, kLeftButton, mouse_position_, 2));
//    events.push_back(CreateWebMouseEvent(
//        kMouseUp, kLeftButton, mouse_position_, 2));
//    error = ProcessWebMouseEvents(events);
//  } else {
//    RunSessionTask(base::Bind(
//        &Automation::MouseDoubleClickDeprecated,
//        base::Unretained(automation_.get()),
//        current_target_.view_id,
//        mouse_position_,
//        &error));
//  }
  RunSessionTask(base::Bind(
          &Automation::MouseDoubleClickDeprecated,
          base::Unretained(automation_.get()),
          current_target_.view_id,
          mouse_position_,
          &error));
  return error;
}

Error* Session::GetCookies(const std::string& url, ListValue** cookies) {
  Error* error = NULL;
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support navigation");
  }
  RunSessionTask(base::Bind(
      &Automation::GetCookies,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      url,
      cookies,
      &error));
  return error;
}

Error* Session::DeleteCookie(const std::string& url,
                           const std::string& cookie_name) {
  Error* error = NULL;
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support navigation");
  }
  RunSessionTask(base::Bind(
      &Automation::DeleteCookie,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      url,
      cookie_name,
      &error));
  return error;
}

// Note that when this is called from CookieCommand::ExecutePost then
// |cookie_dict| is destroyed as soon as the caller finishes. Therefore
// it is essential that RunSessionTask executes synchronously.
Error* Session::SetCookie(const std::string& url,
                          DictionaryValue* cookie_dict) {
  Error* error = NULL;
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support navigation");
  }
  RunSessionTask(base::Bind(
      &Automation::SetCookie,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      url,
      cookie_dict,
      &error));
  return error;
}

Error* Session::GetViews(std::vector<WebViewInfo>* views) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::GetViews,
      base::Unretained(automation_.get()),
      views,
      &error));
  return error;
}

Error* Session::SwitchToView(const std::string& id_or_name) {
  Error* error = NULL;
  bool does_exist = false;

  WebViewId new_view;
  bool isValid = StringToWebViewId(id_or_name, &new_view);
  if (isValid)
  {
    RunSessionTask(base::Bind(
        &Automation::DoesViewExist,
        base::Unretained(automation_.get()),
        &new_view,
        &does_exist,
        &error));
    if (error)
      return error;
  }

  if (!does_exist) {
    // See if any of the tab window names match |name|.
    std::vector<WebViewInfo> views;
    Error* error = GetViews(&views);
    if (error)
      return error;
    for (size_t i = 0; i < views.size(); ++i) {
      if (views[i].view_id.IsTab())
      {
          std::string window_name;
          Error* error = ExecuteScriptAndParse(
              FrameId(views[i].view_id, FramePath()),
              "function() { return window.name; }",
              "getWindowName",
              new ListValue(),
              CreateDirectValueParser(&window_name));
          if (error)
            return error;

          if (id_or_name == window_name) {
            new_view = views[i].view_id;
            does_exist = true;
            break;
          }
      }
      else if (views[i].view_id.IsApp())
      {
          std::string window_name;

          RunSessionTask(base::Bind(
              &Automation::GetViewTitle,
              base::Unretained(automation_.get()),
              views[i].view_id,
              &window_name,
              &error));
          if (error)
            return error;

          if (id_or_name == window_name) {
            new_view = views[i].view_id;
            does_exist = true;
            break;
          }
      }
    }
  }

  if (!does_exist)
    return new Error(kNoSuchWindow);
  frame_elements_.clear();
  current_target_ = FrameId(new_view, FramePath());
  return NULL;
}

Error* Session::SwitchToFrameWithNameOrId(const std::string& name_or_id) {
  std::string script =
      "function(arg) {"
      "  var xpath = '(/html/body//iframe|/html/frameset/frame)';"
      "  var sub = function(s) { return s.replace(/\\$/g, arg); };"
      "  xpath += sub('[@name=\"$\" or @id=\"$\"]');"
      "  return document.evaluate(xpath, document, null, "
      "      XPathResult.FIRST_ORDERED_NODE_TYPE, null).singleNodeValue;"
      "}";
  return SwitchToFrameWithJavaScriptLocatedFrame(
      script, CreateListValueFrom(name_or_id));
}

Error* Session::SwitchToFrameWithIndex(int index) {
  // We cannot simply index into window.frames because we need to know the
  // tagName of the frameElement. If child frame N is from another domain, then
  // the following will run afoul of the same origin policy:
  //   window.frames[N].frameElement;
  // Instead of indexing window.frames, we use an XPath expression to index
  // into the list of all IFRAME and FRAME elements on the page - if we find
  // something, then that XPath expression can be used as the new frame's XPath.
  std::string script =
      "function(index) {"
      "  var xpathIndex = '[' + (index + 1) + ']';"
      "  var xpath = '(/html/body//iframe|/html/frameset/frame)' + "
      "              xpathIndex;"
      "  return document.evaluate(xpath, document, null, "
      "      XPathResult.FIRST_ORDERED_NODE_TYPE, null).singleNodeValue;"
      "}";
  return SwitchToFrameWithJavaScriptLocatedFrame(
      script, CreateListValueFrom(index));
}

Error* Session::SwitchToFrameWithElement(const ElementId& element) {
  // TODO(jleyba): Extract this, and the other frame switch methods to an atom.
  std::string script =
      "function(elem) {"
      "  if (elem.nodeType != 1 || !/^i?frame$/i.test(elem.tagName)) {"
      "    console.error('Element is not a frame');"
      "    return null;"
      "  }"
      "  for (var i = 0; i < window.frames.length; i++) {"
      "    if (elem.contentWindow == window.frames[i]) {"
      "      return elem;"
      "    }"
      "  }"
      "  console.info('Frame is not connected to this DOM tree');"
      "  return null;"
      "}";
  return SwitchToFrameWithJavaScriptLocatedFrame(
      script, CreateListValueFrom(element));
}

void Session::SwitchToTopFrame() {
  frame_elements_.clear();
  current_target_.frame_path = FramePath();
}

Error* Session::SwitchToTopFrameIfCurrentFrameInvalid() {
  std::vector<std::string> components;
  current_target_.frame_path.GetComponents(&components);
  if (frame_elements_.size() != components.size()) {
    return new Error(kUnknownError,
                     "Frame element vector out of sync with frame path");
  }
  FramePath frame_path;
  // Start from the root path and check that each frame element that makes
  // up the current frame target is valid by executing an empty script.
  // This code should not execute script in any frame before making sure the
  // frame element is valid, otherwise the automation hangs until a timeout.
  for (size_t i = 0; i < frame_elements_.size(); ++i) {
    FrameId frame_id(current_target_.view_id, frame_path);
    scoped_ptr<Error> error(ExecuteScriptAndParse(
        frame_id,
        "function(){ }",
        "emptyScript",
        CreateListValueFrom(frame_elements_[i]),
        CreateDirectValueParser(kSkipParsing)));
    if (error.get() && error->code() == kStaleElementReference) {
      SwitchToTopFrame();
    } else if (error.get()) {
      return error.release();
    }
    frame_path = frame_path.Append(components[i]);
  }
  return NULL;
}

Error* Session::CloseWindow() {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::CloseView,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      &error));

  if (!error) {
    std::vector<WebViewInfo> views;
    scoped_ptr<Error> error(GetViews(&views));
    if (error.get() || views.empty()) {
      // The automation connection will soon be closed, if not already,
      // because we supposedly just closed the last window. Terminate the
      // session.
      // TODO(kkania): This will cause us problems if GetWindowIds fails for a
      // reason other than the channel is disconnected. Look into having
      // |GetWindowIds| tell us if it just closed the last window.
      Terminate();
    }
  }
  return error;
}

Error* Session::GetWindowBounds(const WebViewId& window, Rect* bounds) {
    if (window.IsApp()) {
        Error* error = NULL;
        RunSessionTask(base::Bind(
            &Automation::GetViewBounds,
            base::Unretained(automation_.get()),
            window,
            bounds,
            &error));
        return error;
    }

  const char* kGetWindowBoundsScript =
      "function() {"
      "  return {"
      "    'left': window.screenX,"
      "    'top': window.screenY,"
      "    'width': window.outerWidth,"
      "    'height': window.outerHeight"
      "  }"
      "}";
  return ExecuteScriptAndParse(
      FrameId(window, FramePath()),
      kGetWindowBoundsScript,
      "getWindowBoundsScript",
      new ListValue(),
      CreateDirectValueParser(bounds));
}

Error* Session::SetWindowBounds(
    const WebViewId& window,
    const Rect& bounds) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::SetViewBounds,
      base::Unretained(automation_.get()),
      window,
      bounds,
      &error));
  return error;
}

Error* Session::MaximizeWindow(const WebViewId& window) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
        &Automation::MaximizeView,
        base::Unretained(automation_.get()),
        window,
        &error));
  return error;
}

Error* Session::GetAlertMessage(std::string* text) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::GetAppModalDialogMessage,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      text,
      &error));
  return error;
}

Error* Session::SetAlertPromptText(const std::string& alert_prompt_text)
{
  std::string message_text;
  // Only set the alert prompt text if an alert is actually active.
  Error* error = GetAlertMessage(&message_text);
  if (!error)
  {
      RunSessionTask(base::Bind(
          &Automation::SetAlertPromptText,
          base::Unretained(automation_.get()),
          current_target_.view_id,
          alert_prompt_text,
          &error));
      return error;
  }
  return error;
}

Error* Session::AcceptOrDismissAlert(bool accept) {
  Error* error = NULL;
  if (accept && has_alert_prompt_text_) {
    RunSessionTask(base::Bind(
        &Automation::AcceptPromptAppModalDialog,
        base::Unretained(automation_.get()),
        current_target_.view_id,
        alert_prompt_text_,
        &error));
  } else {
    RunSessionTask(base::Bind(
        &Automation::AcceptOrDismissAppModalDialog,
        base::Unretained(automation_.get()),
        current_target_.view_id,
        accept,
        &error));
  }
  has_alert_prompt_text_ = false;
  return error;
}

std::string Session::GetBrowserVersion() {
  std::string version;
  version = QT_VERSION_STR;
  return version;
}

Error* Session::CompareBrowserVersion(int client_build_no,
                                      int client_patch_no,
                                      bool* is_newer_or_equal) {
//  std::string version = GetBrowserVersion();
//  std::vector<std::string> split_version;
//  base::SplitString(version, '.', &split_version);
//  if (split_version.size() != 4) {
//    return new Error(
//        kUnknownError, "Browser version has unrecognized format: " + version);
//  }
//  int build_no, patch_no;
//  if (!base::StringToInt(split_version[2], &build_no) ||
//      !base::StringToInt(split_version[3], &patch_no)) {
//    return new Error(
//        kUnknownError, "Browser version has unrecognized format: " + version);
//  }
//  if (build_no < client_build_no)
//    *is_newer_or_equal = false;
//  else if (build_no > client_build_no)
//    *is_newer_or_equal = true;
//  else
//    *is_newer_or_equal = patch_no >= client_patch_no;
  return NULL;
}

Error* Session::FindElement(const FrameId& frame_id,
                            const ElementId& root_element,
                            const std::string& locator,
                            const std::string& query,
                            ElementId* element) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::FindNativeElement,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            root_element,
            locator,
            query,
            element,
            &error));

        return error;
    }

    std::vector<ElementId> elements;
    Error* error = FindElementsHelper(
        frame_id, root_element, locator, query, true, &elements);
    if (!error)
      *element = elements[0];
    return error;
}

Error* Session::FindElements(const FrameId& frame_id,
                             const ElementId& root_element,
                             const std::string& locator,
                             const std::string& query,
                             std::vector<ElementId>* elements) {
    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::FindNativeElements,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            root_element,
            locator,
            query,
            elements,
            &error));

        return error;
    }

  return FindElementsHelper(
      frame_id, root_element, locator, query, false, elements);
}

Error* Session::GetElementLocationInView(
    const ElementId& element,
    Point* location) {

    if (current_target_.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetNativeElementLocationInView,
            base::Unretained(automation_.get()),
            current_target_.view_id,
            element,
            location,
            &error));

        return error;
    }

  Size size;
  Error* error = GetElementSize(current_target_, element, &size);
  if (error)
    return error;
  return GetElementRegionInView(
      element, Rect(Point(0, 0), size),
      false /* center */, false /* verify_clickable_at_middle */, location);
}

Error* Session::GetElementLocation(
        const FrameId& frame_id,
        const ElementId& element,
        Point* location) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetNativeElementLocation,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            location,
            &error));

        return error;
    }


  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::GET_LOCATION),
      "getLocation",
      CreateListValueFrom(element),
      CreateDirectValueParser(location));
}

Error* Session::ElementEquals(const FrameId& frame_id,
    const ElementId& element1,
    const ElementId& element2,
    bool* is_equal) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::NativeElementEquals,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element1,
            element2,
            is_equal,
            &error));

        return error;
    }

    std::string script = "function(el1, el2) { return el1 == el2; }";
    //std::string script = "return arguments[0] == arguments[1];";

  return ExecuteScriptAndParse(
      frame_id,
      script,
      "elementEquals",
      CreateListValueFrom(element1, element2),
      CreateDirectValueParser(is_equal));
}

Error* Session::GetElementRegionInView(
    const ElementId& element,
    const Rect& region,
    bool center,
    bool verify_clickable_at_middle,
    Point* location) {

  CHECK(element.is_valid());

  Point region_offset = region.origin();
  Size region_size = region.size();
  Error* error = GetElementRegionInViewHelper(
      current_target_, element, region, center, verify_clickable_at_middle,
      &region_offset);
  if (error)
    return error;

  for (FramePath frame_path = current_target_.frame_path;
       frame_path.IsSubframe();
       frame_path = frame_path.Parent()) {
    // Find the frame element for the current frame path.
    FrameId frame_id(current_target_.view_id, frame_path.Parent());
    ElementId frame_element;
    std::string frameBasePath = base::StringPrintf("//*[@wd_frame_id_ = '%s']", frame_path.BaseName().value().c_str());
    error = FindElement(
        frame_id, ElementId(""),
        LocatorType::kXpath, frameBasePath, &frame_element);
    if (error) {
      std::string context = base::StringPrintf(
          "Could not find frame element (%s) in frame (%s)",
          frame_path.BaseName().value().c_str(),
          frame_path.Parent().value().c_str());
      error->AddDetails(context);
      return error;
    }
    // Modify |region_offset| by the frame's border.
    int border_left, border_top;
    error = GetElementBorder(
        frame_id, frame_element, &border_left, &border_top);
    if (error)
      return error;
    region_offset.Offset(border_left, border_top);

    error = GetElementRegionInViewHelper(
        frame_id, frame_element, Rect(region_offset, region_size),
        center, verify_clickable_at_middle, &region_offset);
    if (error)
      return error;
  }
  *location = region_offset;
  return NULL;
}

Error* Session::GetElementSize(const FrameId& frame_id,
                               const ElementId& element,
                               Size* size) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetNativeElementSize,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            size,
            &error));

        return error;
    }

  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::GET_SIZE),
      "getSize",
      CreateListValueFrom(element),
      CreateDirectValueParser(size));
}

Error* Session::GetElementFirstClientRect(const FrameId& frame_id,
                                          const ElementId& element,
                                          Rect* rect) {

  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::GET_FIRST_CLIENT_RECT),
      "getFirstClientRect",
      CreateListValueFrom(element),
      CreateDirectValueParser(rect));
}

Error* Session::GetElementEffectiveStyle(
    const FrameId& frame_id,
    const ElementId& element,
    const std::string& prop,
    std::string* value) {

  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::GET_EFFECTIVE_STYLE),
      "getEffectiveStyle",
      CreateListValueFrom(element, prop),
      CreateDirectValueParser(value));
}

Error* Session::GetElementBorder(const FrameId& frame_id,
                                 const ElementId& element,
                                 int* border_left,
                                 int* border_top) {

  std::string border_left_str, border_top_str;
  Error* error = GetElementEffectiveStyle(
      frame_id, element, "border-left-width", &border_left_str);
  if (error)
    return error;
  error = GetElementEffectiveStyle(
      frame_id, element, "border-top-width", &border_top_str);
  if (error)
    return error;

  base::StringToInt(border_left_str, border_left);
  base::StringToInt(border_top_str, border_top);
  return NULL;
}

Error* Session::IsElementDisplayed(const FrameId& frame_id,
                                   const ElementId& element,
                                   bool ignore_opacity,
                                   bool* is_displayed) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::IsNativeElementDisplayed,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            ignore_opacity,
            is_displayed,
            &error));

        return error;
    }

  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::IS_DISPLAYED),
      "isDisplayed",
      CreateListValueFrom(element, ignore_opacity),
      CreateDirectValueParser(is_displayed));
}

Error* Session::ActiveElement(const FrameId& frame_id,
                   ElementId* element) {
    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetNativeElementWithFocus,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            &error));

        return error;
    }

    std::string script = "function() { return document.activeElement || document.body; }";
    //"return document.activeElement || document.body"

  return ExecuteScriptAndParse(
      frame_id,
      script,
      "activeElement",
      new ListValue(),
      CreateDirectValueParser(element));
}



Error* Session::ClearElement(const FrameId& frame_id,
                          const ElementId& element) {
    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::ClearNativeElement,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            &error));

        return error;
    }

    ListValue args;
    args.Append(element.ToValue());

    std::string script = base::StringPrintf(
        "(%s).apply(null, arguments);", atoms::asString(atoms::CLEAR).c_str());

    Value* result = NULL;
    Error* error = ExecuteScript(script, &args, &result);

    // TODO: check if we need return "result" value
    return error;
}

Error* Session::MoveAndClickElement(const FrameId& frame_id,
                                const ElementId& element) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;
        Point location;

        error = GetClickableLocation(element, &location);
        if (!error)
          error = MouseMoveAndClick(location, automation::kLeftButton);

        return error;
    }

    std::string tag_name;
    Error* error = GetElementTagName(
            frame_id, element, &tag_name);
    if (error) {
      return error;
    }

    if (tag_name == "option") {
      const char* kCanOptionBeToggledScript =
          "function(option) {"
          "  for (var parent = option.parentElement;"
          "       parent;"
          "       parent = parent.parentElement) {"
          "    if (parent.tagName.toLowerCase() == 'select') {"
          "      return parent.multiple;"
          "    }"
          "  }"
          "  throw new Error('Option element is not in a select');"
          "}";
      bool can_be_toggled;
      error = ExecuteScriptAndParse(
          frame_id,
          kCanOptionBeToggledScript,
          "canOptionBeToggled",
          CreateListValueFrom(element),
          CreateDirectValueParser(&can_be_toggled));
      if (error) {
        return error;
      }

      if (can_be_toggled) {
        error = ToggleOptionElement(
            frame_id, element);
      } else {
        error = SetOptionElementSelected(
            frame_id, element, true);
      }
    } else {
      Point location;
      error = GetClickableLocation(element, &location);
      if (!error)
        error = MouseMoveAndClick(location, automation::kLeftButton);
    }

    return error;
}

Error* Session::IsElementEnabled(const FrameId& frame_id,
                                 const ElementId& element,
                                 bool* is_enabled) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::IsNativeElementEnabled,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            is_enabled,
            &error));

        return error;
    }

  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::IS_ENABLED),
      "isEnabled",
      CreateListValueFrom(element),
      CreateDirectValueParser(is_enabled));
}

Error* Session::IsOptionElementSelected(const FrameId& frame_id,
                                        const ElementId& element,
                                        bool* is_selected) {

    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::IsNativeElementSelected,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            is_selected,
            &error));

        return error;
    }

  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::IS_SELECTED),
      "isSelected",
      CreateListValueFrom(element),
      CreateDirectValueParser(is_selected));
}

Error* Session::SetOptionElementSelected(const FrameId& frame_id,
                                         const ElementId& element,
                                         bool selected) {

    if (!current_target_.view_id.IsTab()) {
      return new Error(kUnknownError,
                       "The current target does not support option elements");
    }

  // This wrapper ensures the script is started successfully and
  // allows for an alert to happen when the option selection occurs.
  // See selenium bug 2671.
  const char kSetSelectedWrapper[] =
      "var args = [].slice.apply(arguments);"
      "args[args.length - 1]();"
      "return (%s).apply(null, args.slice(0, args.length - 1));";
  Value* value = NULL;
  Error* error = ExecuteAsyncScript(
      frame_id,
      base::StringPrintf(kSetSelectedWrapper,
                         atoms::asString(atoms::CLICK).c_str()),
      CreateListValueFrom(element, selected),
      &value);
  scoped_ptr<Value> scoped_value(value);
  return error;
}

Error* Session::ToggleOptionElement(const FrameId& frame_id,
                                    const ElementId& element) {
  bool is_selected;
  Error* error = IsOptionElementSelected(frame_id, element, &is_selected);
  if (error)
    return error;

  return SetOptionElementSelected(frame_id, element, !is_selected);
}

Error* Session::GetElementTagName(const FrameId& frame_id,
                                  const ElementId& element,
                                  std::string* tag_name) {
    if (!frame_id.view_id.IsTab()) {
      return new Error(kUnknownError,
                       "The current target does not support tag names");
    }
  return ExecuteScriptAndParse(
      frame_id,
      "function(elem) { return elem.tagName.toLowerCase() }",
      "getElementTagName",
      CreateListValueFrom(element),
      CreateDirectValueParser(tag_name));
}

Error* Session::GetElementText(const FrameId& frame_id,
                         const ElementId& element,
                         std::string* element_text) {
    if (frame_id.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetNativeElementText,
            base::Unretained(automation_.get()),
            frame_id.view_id,
            element,
            element_text,
            &error));

        return error;
   }

  return ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::GET_TEXT),
      "getText",
      CreateListValueFrom(element),
      CreateDirectValueParser(element_text));


}


Error* Session::GetClickableLocation(const ElementId& element,
                                     Point* location) {

    if (current_target_.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetNativeElementClickableLocation,
            base::Unretained(automation_.get()),
            current_target_.view_id,
            element,
            location,
            &error));

        return error;
    }

  bool is_displayed = false;
  Error* error = IsElementDisplayed(
      current_target_, element, true /* ignore_opacity */, &is_displayed);
  if (error)
    return error;
  if (!is_displayed)
    return new Error(kElementNotVisible, "Element must be displayed to click");

  // We try 3 methods to determine clickable location. This mostly follows
  // what FirefoxDriver does. Try the first client rect, then the bounding
  // client rect, and lastly the size of the element (via closure).
  // SVG is one case that doesn't have a first client rect.
  Rect rect;
  scoped_ptr<Error> ignore_error(
      GetElementFirstClientRect(current_target_, element, &rect));
  if (ignore_error.get()) {
    Rect client_rect;
    ignore_error.reset(ExecuteScriptAndParse(
        current_target_,
        "function(elem) { return elem.getBoundingClientRect() }",
        "getBoundingClientRect",
        CreateListValueFrom(element),
        CreateDirectValueParser(&client_rect)));
    rect = Rect(0, 0, client_rect.width(), client_rect.height());
  }
  if (ignore_error.get()) {
    Size size;
    ignore_error.reset(GetElementSize(current_target_, element, &size));
    rect = Rect(0, 0, size.width(), size.height());
  }
  if (ignore_error.get()) {
    return new Error(kUnknownError,
                     "Unable to determine clickable location of element");
  }
  error = GetElementRegionInView(
      element, rect, true /* center */, true /* verify_clickable_at_middle */,
      location);
  if (error)
    return error;
  location->Offset(rect.width() / 2, rect.height() / 2);
  return NULL;
}

Error* Session::GetAttribute(const ElementId& element,
                             const std::string& key,
                             Value** value) {
    if (current_target_.view_id.IsApp()) {
        Error* error = NULL;

        RunSessionTask(base::Bind(
            &Automation::GetNativeElementProperty,
            base::Unretained(automation_.get()),
            current_target_.view_id,
            element,
            key,
            value,
            &error));

        return error;
    }

  return ExecuteScriptAndParse(
      current_target_,
      atoms::asString(atoms::GET_ATTRIBUTE),
      "getAttribute",
      CreateListValueFrom(element, key),
      CreateDirectValueParser(value));
}

Error* Session::WaitForAllViewsToStopLoading() {
  if (!automation_.get())
    return NULL;

  logger_.Log(kFinerLogLevel, "Waiting for all views to stop loading...");
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::WaitForAllViewsToStopLoading,
      base::Unretained(automation_.get()),
      &error));
  logger_.Log(kFinerLogLevel, "Done waiting for all views to stop loading");
  return error;
}

Error* Session::InstallExtension(
    const FilePath& path, std::string* extension_id) {
//  Error* error = NULL;
//  RunSessionTask(base::Bind(
//      &Automation::InstallExtension,
//      base::Unretained(automation_.get()),
//      path,
//      extension_id,
//      &error));
//  return error;
}

Error* Session::GetExtensionsInfo(base::ListValue* extensions_list) {
//  Error* error = NULL;
//  RunSessionTask(base::Bind(
//      &Automation::GetExtensionsInfo,
//      base::Unretained(automation_.get()),
//      extensions_list,
//      &error));
//  return error;
}

Error* Session::IsPageActionVisible(
    const WebViewId& tab_id,
    const std::string& extension_id,
    bool* is_visible) {
//  if (!tab_id.IsTab()) {
//    return new Error(
//        kUnknownError,
//        "The current target does not support page actions. Switch to a tab.");
//  }
//  Error* error = NULL;
//  RunSessionTask(base::Bind(
//      &Automation::IsPageActionVisible,
//      base::Unretained(automation_.get()),
//      tab_id,
//      extension_id,
//      is_visible,
//      &error));
//  return error;
}

Error* Session::SetExtensionState(
    const std::string& extension_id, bool enable) {
//  Error* error = NULL;
//  RunSessionTask(base::Bind(
//      &Automation::SetExtensionState,
//      base::Unretained(automation_.get()),
//      extension_id,
//      enable,
//      &error));
//  return error;
}

Error* Session::ClickExtensionButton(
    const std::string& extension_id, bool browser_action) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::ClickExtensionButton,
      base::Unretained(automation_.get()),
      extension_id,
      browser_action,
      &error));
  return error;
}

Error* Session::UninstallExtension(const std::string& extension_id) {
//  Error* error = NULL;
//  RunSessionTask(base::Bind(
//      &Automation::UninstallExtension,
//      base::Unretained(automation_.get()),
//      extension_id,
//      &error));
//  return error;
}

Error* Session::SetPreference(
    const std::string& pref,
    bool is_user_pref,
    base::Value* value) {
  Error* error = NULL;
//  if (is_user_pref) {
//    RunSessionTask(base::Bind(
//        &Automation::SetPreference,
//        base::Unretained(automation_.get()),
//        pref,
//        value,
//        &error));
//    if (error)
//      error->AddDetails("Failed to set user pref '" + pref + "'");
//  } else {
//    RunSessionTask(base::Bind(
//        &Automation::SetLocalStatePreference,
//        base::Unretained(automation_.get()),
//        pref,
//        value,
//        &error));
//    if (error)
//      error->AddDetails("Failed to set local state pref '" + pref + "'");
//  }
//  return error;
}

base::ListValue* Session::GetLog() const {
  return session_log_->entries_list()->DeepCopy();
}

Error* Session::GetBrowserConnectionState(bool* online) {
  return ExecuteScriptAndParse(
      current_target_,
      atoms::asString(atoms::IS_ONLINE),
      "isOnline",
      new ListValue(),
      CreateDirectValueParser(online));
}

Error* Session::GetAppCacheStatus(int* status) {
  return ExecuteScriptAndParse(
      current_target_,
      atoms::asString(atoms::GET_APPCACHE_STATUS),
      "getAppcacheStatus",
      new ListValue(),
      CreateDirectValueParser(status));
}

Error* Session::GetStorageSize(StorageType type, int* size) {
  std::string js = atoms::asString(
      type == kLocalStorageType ? atoms::GET_LOCAL_STORAGE_SIZE
                                : atoms::GET_SESSION_STORAGE_SIZE);
  return ExecuteScriptAndParse(
      current_target_,
      js,
      "getStorageSize",
      new ListValue(),
      CreateDirectValueParser(size));
}

Error* Session::SetStorageItem(StorageType type,
                               const std::string& key,
                               const std::string& value) {
  std::string js = atoms::asString(
      type == kLocalStorageType ? atoms::SET_LOCAL_STORAGE_ITEM
                                : atoms::SET_SESSION_STORAGE_ITEM);
  return ExecuteScriptAndParse(
      current_target_,
      js,
      "setStorageItem",
      CreateListValueFrom(key, value),
      CreateDirectValueParser(kSkipParsing));
}

Error* Session::ClearStorage(StorageType type) {
  std::string js = atoms::asString(
      type == kLocalStorageType ? atoms::CLEAR_LOCAL_STORAGE
                                : atoms::CLEAR_SESSION_STORAGE);
  return ExecuteScriptAndParse(
      current_target_,
      js,
      "clearStorage",
      new ListValue(),
      CreateDirectValueParser(kSkipParsing));
}

Error* Session::GetStorageKeys(StorageType type, ListValue** keys) {
  std::string js = atoms::asString(
      type == kLocalStorageType ? atoms::GET_LOCAL_STORAGE_KEYS
                                : atoms::GET_SESSION_STORAGE_KEYS);
  return ExecuteScriptAndParse(
      current_target_,
      js,
      "getStorageKeys",
      new ListValue(),
      CreateDirectValueParser(keys));
}

Error* Session::GetStorageItem(StorageType type,
                               const std::string& key,
                               std::string* value) {
  std::string js = atoms::asString(
      type == kLocalStorageType ? atoms::GET_LOCAL_STORAGE_ITEM
                                : atoms::GET_SESSION_STORAGE_ITEM);
  return ExecuteScriptAndParse(
      current_target_,
      js,
      "getStorageItem",
      CreateListValueFrom(key),
      CreateDirectValueParser(value));
}

Error* Session::RemoveStorageItem(StorageType type,
                                  const std::string& key,
                                  std::string* value) {
  std::string js = atoms::asString(
      type == kLocalStorageType ? atoms::REMOVE_LOCAL_STORAGE_ITEM
                                : atoms::REMOVE_SESSION_STORAGE_ITEM);
  return ExecuteScriptAndParse(
      current_target_,
      js,
      "removeStorageItem",
      CreateListValueFrom(key),
      CreateDirectValueParser(value));
}

Error* Session::GetGeolocation(
    scoped_ptr<base::DictionaryValue>* geolocation) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::GetGeolocation,
      base::Unretained(automation_.get()),
      geolocation,
      &error));
  return error;
}

Error* Session::OverrideGeolocation(const base::DictionaryValue* geolocation) {
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::OverrideGeolocation,
      base::Unretained(automation_.get()),
      geolocation,
      &error));
  return error;
}

const std::string& Session::id() const {
  return id_;
}

const FrameId& Session::current_target() const {
  return current_target_;
}

void Session::set_async_script_timeout(int timeout_ms) {
  async_script_timeout_ = timeout_ms;
}

int Session::async_script_timeout() const {
  return async_script_timeout_;
}

void Session::set_implicit_wait(int timeout_ms) {
  implicit_wait_ = timeout_ms;
}

int Session::implicit_wait() const {
  return implicit_wait_;
}

const Point& Session::get_mouse_position() const {
  return mouse_position_;
}

const Logger& Session::logger() const {
  return logger_;
}

const FilePath& Session::temp_dir() const {
  return temp_dir_.path();
}

const Capabilities& Session::capabilities() const {
  return capabilities_;
}

void Session::RunSessionTask(const base::Closure& task) {
  base::WaitableEvent done_event(false, false);
  thread_.message_loop_proxy()->PostTask(FROM_HERE, base::Bind(
      &Session::RunClosureOnSessionThread,
      base::Unretained(this),
      task,
      &done_event));
  // See SetCookie for why it is essential that we wait here.
  done_event.Wait();
}

void Session::RunClosureOnSessionThread(const base::Closure& task,
                                        base::WaitableEvent* done_event) {
    QMetaObject::invokeMethod(&qtask, "runTask", Qt::BlockingQueuedConnection, Q_ARG(const base::Closure&, task));
    done_event->Signal();
}

void Session::InitOnSessionThread(const Automation::BrowserOptions& options,
                                  int* build_no,
                                  Error** error) {
  automation_.reset(new Automation(logger_));
  automation_->Init(options, build_no, error);
  if (*error)
    return;

  std::vector<WebViewInfo> views;
  automation_->GetViews(&views, error);
  if (*error)
    return;
  if (views.empty()) {
    *error = new Error(kUnknownError, "No view ids after initialization");
    return;
  }
  current_target_ = FrameId(views[0].view_id, FramePath());
}

void Session::TerminateOnSessionThread() {
  if (automation_.get())
    automation_->Terminate();
  automation_.reset();
}

Error* Session::ExecuteScriptAndParseValue(const FrameId& frame_id,
                                           const std::string& script,
                                           Value** script_result, bool isAsync) {
  std::string response_json;
  Error* error = NULL;
  RunSessionTask(base::Bind(
      &Automation::ExecuteScript,
      base::Unretained(automation_.get()),
      frame_id.view_id,
      frame_id.frame_path,
      script,
      &response_json,
      isAsync,
      &error));
  if (error)
    return error;

  scoped_ptr<Value> value(base::JSONReader::ReadAndReturnError(
      response_json, base::JSON_ALLOW_TRAILING_COMMAS, NULL, NULL));
  if (!value.get())
    return new Error(kUnknownError, "Failed to parse script result");
  if (value->GetType() != Value::TYPE_DICTIONARY)
    return new Error(kUnknownError, "Execute script returned non-dict: " +
                         JsonStringify(value.get()));
  DictionaryValue* result_dict = static_cast<DictionaryValue*>(value.get());

  int status;
  if (!result_dict->GetInteger("status", &status))
    return new Error(kUnknownError, "Execute script did not return status: " +
                         JsonStringify(result_dict));
  ErrorCode code = static_cast<ErrorCode>(status);
  if (code != kSuccess) {
    DictionaryValue* error_dict;
    std::string error_msg;
    if (result_dict->GetDictionary("value", &error_dict))
      error_dict->GetString("message", &error_msg);
    if (error_msg.empty())
      error_msg = "Script failed with error code: " + base::IntToString(code);
    return new Error(code, error_msg);
  }

  Value* tmp;
  if (result_dict->Get("value", &tmp)) {
    *script_result= tmp->DeepCopy();
  } else {
    // "value" was not defined in the returned dictionary; set to null.
    *script_result= Value::CreateNullValue();
  }
  return NULL;
}

void Session::SendKeysOnSessionThread(const string16& keys,
                                      bool release_modifiers, Error** error) {
  std::vector<WebKeyEvent> key_events;
  std::string error_msg;
  if (!ConvertKeysToWebKeyEvents(keys, logger_, release_modifiers,
                                 &sticky_modifiers_, &key_events, &error_msg)) {
    *error = new Error(kUnknownError, error_msg);
    return;
  }
  for (size_t i = 0; i < key_events.size(); ++i) {
    if (capabilities_.native_events) {
      // The automation provider will generate up/down events for us, we
      // only need to call it once as compared to the WebKeyEvent method.
      // Hence we filter events by their types, keeping only rawkeydown.
//      if (key_events[i].type != automation::kRawKeyDownType)
//        continue;
//      automation_->SendNativeKeyEvent(
//          current_target_.view_id,
//          key_events[i].key_code,
//          key_events[i].modifiers,
//          error);
    } else {
      automation_->SendWebKeyEvent(
          current_target_.view_id,
          key_events[i], error);
    }
    if (*error) {
      std::string details = base::StringPrintf(
          "Failed to send key event. Event details:\n"
              "Type: %d, KeyCode: %d, UnmodifiedText: %s, ModifiedText: %s, "
              "Modifiers: %d",
          key_events[i].type,
          key_events[i].key_code,
          key_events[i].unmodified_text.c_str(),
          key_events[i].modified_text.c_str(),
          key_events[i].modifiers);
      (*error)->AddDetails(details);
      return;
    }
  }
}

void Session::SendKeysOnElementSessionThread(const ElementId& element,
                                      const string16& keys,
                                      bool release_modifiers,
                                      Error** error) {
  std::vector<WebKeyEvent> key_events;
  std::string error_msg;
  if (!ConvertKeysToWebKeyEvents(keys, logger_, release_modifiers,
                                 &sticky_modifiers_, &key_events, &error_msg)) {
    *error = new Error(kUnknownError, error_msg);
    return;
  }
  for (size_t i = 0; i < key_events.size(); ++i) {
    if (capabilities_.native_events) {
      // The automation provider will generate up/down events for us, we
      // only need to call it once as compared to the WebKeyEvent method.
      // Hence we filter events by their types, keeping only rawkeydown.
//      if (key_events[i].type != automation::kRawKeyDownType)
//        continue;
//      automation_->SendNativeKeyEvent(
//          current_target_.view_id,
//          key_events[i].key_code,
//          key_events[i].modifiers,
//          error);
    } else {
      automation_->SendNativeElementWebKeyEvent(
          current_target_.view_id,
          element,
          key_events[i], error);
    }
    if (*error) {
      std::string details = base::StringPrintf(
          "Failed to send key event. Event details:\n"
              "Type: %d, KeyCode: %d, UnmodifiedText: %s, ModifiedText: %s, "
              "Modifiers: %d",
          key_events[i].type,
          key_events[i].key_code,
          key_events[i].unmodified_text.c_str(),
          key_events[i].modified_text.c_str(),
          key_events[i].modifiers);
      (*error)->AddDetails(details);
      return;
    }
  }
}

//Error* Session::ProcessWebMouseEvents(
//    const std::vector<WebMouseEvent>& events) {
//  for (size_t i = 0; i < events.size(); ++i) {
//    Error* error = NULL;
//    RunSessionTask(base::Bind(
//        &Automation::SendWebMouseEvent,
//        base::Unretained(automation_.get()),
//        current_target_.view_id,
//        events[i],
//        &error));
//    if (error)
//      return error;
//    mouse_position_ = Point(events[i].x, events[i].y);
//  }
//  return NULL;
//}

WebMouseEvent Session::CreateWebMouseEvent(
    automation::MouseEventType type,
    automation::MouseButton button,
    const Point& point,
    int click_count) {
  return WebMouseEvent(type, button, point.rounded_x(), point.rounded_y(),
                       click_count, sticky_modifiers_);
}

Error* Session::SwitchToFrameWithJavaScriptLocatedFrame(
    const std::string& script, ListValue* args) {
  class SwitchFrameValueParser : public ValueParser {
   public:
    SwitchFrameValueParser(
        bool* found_frame, ElementId* frame)
        : found_frame_(found_frame), frame_(frame) { }

    virtual ~SwitchFrameValueParser() { }

    virtual bool Parse(base::Value* value) const OVERRIDE {
      if (value->IsType(Value::TYPE_NULL)) {
        *found_frame_ = false;
        return true;
      }
      ElementId id(value);
      if (!id.is_valid()) {
        return false;
      }
      *frame_ = id;
      *found_frame_ = true;
      return true;
    }

   private:
    bool* found_frame_;
    ElementId* frame_;
  };

  bool found_frame;
  ElementId new_frame_element;
  Error* error = ExecuteScriptAndParse(
      current_target_, script, "switchFrame", args,
      new SwitchFrameValueParser(&found_frame, &new_frame_element));
  if (error)
    return error;

  if (!found_frame)
    return new Error(kNoSuchFrame);

  std::string frame_id = GenerateRandomID();
  error = ExecuteScriptAndParse(
      current_target_,
              "function(elem, id) { var meta; elem.setAttribute('wd_frame_id_', id); var doc = elem.contentDocument? elem.contentDocument: elem.contentWindow.document; meta=doc.createElement('meta'); meta.name = 'wd_frame_id_'; meta.content = id; var child = doc.body.appendChild(meta);  console.log(meta); console.log(child);}",
      "setFrameId",
      CreateListValueFrom(new_frame_element, frame_id),
      CreateDirectValueParser(kSkipParsing));
  if (error)
    return error;

  RunSessionTask(base::Bind(
     &Automation::AddIdToCurrentFrame,
     base::Unretained(automation_.get()),
     current_target_.view_id,
     FramePath(frame_id),
     &error));
  if (error)
    return error;
  frame_elements_.push_back(new_frame_element);
  current_target_.frame_path = current_target_.frame_path.Append(
      base::StringPrintf("%s", frame_id.c_str()));
  return NULL;
}

Error* Session::FindElementsHelper(const FrameId& frame_id,
                                   const ElementId& root_element,
                                   const std::string& locator,
                                   const std::string& query,
                                   bool find_one,
                                   std::vector<ElementId>* elements) {
  CHECK(root_element.is_valid());
  base::Time start_time = base::Time::Now();
  while (true) {
    std::vector<ElementId> temp_elements;
    Error* error = ExecuteFindElementScriptAndParse(
        frame_id, root_element, locator, query, find_one, &temp_elements);
    if (error)
      return error;

    if (temp_elements.size() > 0u) {
      elements->swap(temp_elements);
      break;
    }

    if ((base::Time::Now() - start_time).InMilliseconds() > implicit_wait_) {
      if (find_one)
        return new Error(kNoSuchElement);
      break;
    }
    base::PlatformThread::Sleep(base::TimeDelta::FromMilliseconds(50));
  }
  return NULL;
}

Error* Session::ExecuteFindElementScriptAndParse(
    const FrameId& frame_id,
    const ElementId& root_element,
    const std::string& locator,
    const std::string& query,
    bool find_one,
    std::vector<ElementId>* elements) {
  CHECK(root_element.is_valid());

  class FindElementsParser : public ValueParser {
   public:
    explicit FindElementsParser(std::vector<ElementId>* elements)
        : elements_(elements) { }

    virtual ~FindElementsParser() { }

    virtual bool Parse(base::Value* value) const OVERRIDE {
      if (!value->IsType(Value::TYPE_LIST))
        return false;
      ListValue* list = static_cast<ListValue*>(value);
      for (size_t i = 0; i < list->GetSize(); ++i) {
        ElementId element;
        Value* element_value = NULL;
        if (!list->Get(i, &element_value))
          return false;
        if (!SetFromValue(element_value, &element))
          return false;
        elements_->push_back(element);
      }
      return true;
    }
   private:
    std::vector<ElementId>* elements_;
  };

  class FindElementParser : public ValueParser {
   public:
    explicit FindElementParser(std::vector<ElementId>* elements)
        : elements_(elements) { }

    virtual ~FindElementParser() { }

    virtual bool Parse(base::Value* value) const OVERRIDE {
      if (value->IsType(Value::TYPE_NULL))
        return true;
      ElementId element;
      bool set = SetFromValue(value, &element);
      if (set)
        elements_->push_back(element);
      return set;
    }
   private:
    std::vector<ElementId>* elements_;
  };

  DictionaryValue locator_dict;
  locator_dict.SetString(locator, query);
  std::vector<ElementId> temp_elements;
  Error* error = NULL;
  if (find_one) {
    error = ExecuteScriptAndParse(
          frame_id,
          atoms::asString(atoms::FIND_ELEMENT),
          "findElement",
          CreateListValueFrom(&locator_dict, root_element),
          new FindElementParser(&temp_elements));
  } else {
    error = ExecuteScriptAndParse(
          frame_id,
          atoms::asString(atoms::FIND_ELEMENTS),
          "findElements",
          CreateListValueFrom(&locator_dict, root_element),
          new FindElementsParser(&temp_elements));
  }
  if (!error)
    elements->swap(temp_elements);
  return error;
}

Error* Session::VerifyElementIsClickable(
    const FrameId& frame_id,
    const ElementId& element,
    const Point& location) {
  class IsElementClickableParser : public ValueParser {
   public:
    IsElementClickableParser(bool* clickable, std::string* message)
        : clickable_(clickable), message_(message) { }

    virtual ~IsElementClickableParser() { }

    virtual bool Parse(base::Value* value) const OVERRIDE {
      if (!value->IsType(Value::TYPE_DICTIONARY))
        return false;
      DictionaryValue* dict = static_cast<DictionaryValue*>(value);
      dict->GetString("message", message_);
      return dict->GetBoolean("clickable", clickable_);
    }

   private:
    bool* clickable_;
    std::string* message_;
  };

  bool clickable;
  std::string message;
  Error* error = ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::IS_ELEMENT_CLICKABLE),
      "isElementClickable",
      CreateListValueFrom(element, location),
      new IsElementClickableParser(&clickable, &message));
  if (error)
    return error;

  if (!clickable) {
    if (message.empty())
      message = "element is not clickable";
    return new Error(kUnknownError, message);
  }
  if (message.length()) {
    logger_.Log(kWarningLogLevel, message);
  }
  return NULL;
}

Error* Session::GetElementRegionInViewHelper(
    const FrameId& frame_id,
    const ElementId& element,
    const Rect& region,
    bool center,
    bool verify_clickable_at_middle,
    Point* location) {
  Point temp_location;
  Error* error = ExecuteScriptAndParse(
      frame_id,
      atoms::asString(atoms::GET_LOCATION_IN_VIEW),
      "getLocationInView",
      CreateListValueFrom(element, center, region),
      CreateDirectValueParser(&temp_location));

  if (verify_clickable_at_middle) {
    Point middle_point = temp_location;
    middle_point.Offset(region.width() / 2, region.height() / 2);
    error = VerifyElementIsClickable(frame_id, element, middle_point);
    if (error)
      return error;
  }
  *location = temp_location;
  return NULL;
}

Error* Session::GetScreenShot(std::string* png) {
  if (!current_target_.view_id.IsTab()) {
    return new Error(kUnknownError,
                     "The current target does not support screenshot");
  }
  Error* error = NULL;

  FilePath path("/tmp/screen.png");

  RunSessionTask(base::Bind(
      &Automation::CaptureEntirePageAsPNG,
      base::Unretained(automation_.get()),
      current_target_.view_id,
      path,
      &error));

  if (error)
    return error;

  if (!file_util::ReadFileToString(path, png))
    return new Error(kUnknownError, "Could not read screenshot file");
  return NULL;
}

#if !defined(NO_TCMALLOC) && (defined(OS_LINUX) || defined(OS_CHROMEOS))
Error* Session::HeapProfilerDump(const std::string& reason) {
  // TODO(dmikurube): Support browser processes.
//  Error* error = NULL;
//  RunSessionTask(base::Bind(
//      &Automation::HeapProfilerDump,
//      base::Unretained(automation_.get()),
//      current_target_.view_id,
//      reason,
//      &error));
//  return error;
}
#endif  // !defined(NO_TCMALLOC) && (defined(OS_LINUX) || defined(OS_CHROMEOS))

Error* Session::PostBrowserStartInit() {
  Error* error = NULL;
  if (!capabilities_.no_website_testing_defaults)
    error = InitForWebsiteTesting();
  if (!error)
    error = SetPrefs();
  if (error)
    return error;

  // Install extensions.
  for (size_t i = 0; i < capabilities_.extensions.size(); ++i) {
    std::string extension_id;
    error = InstallExtension(capabilities_.extensions[i], &extension_id);
    if (error)
      return error;
  }
  return NULL;
}

Error* Session::InitForWebsiteTesting() {
  bool has_prefs_api = false;
  // Don't set these prefs for Chrome 14 and below.
  // TODO(kkania): Remove this when Chrome 14 is unsupported.
  Error* error = CompareBrowserVersion(874, 0, &has_prefs_api);
  if (error || !has_prefs_api)
    return error;

  // Disable checking for SSL certificate revocation.
  error = SetPreference(
      "ssl.rev_checking.enabled",
      false /* is_user_pref */,
      Value::CreateBooleanValue(false));
  if (error)
    return error;

  // Allow content by default.
  // Media-stream cannot be enabled by default; we must specify
  // particular host patterns and devices.
  DictionaryValue* devices = new DictionaryValue();
  devices->SetString("audio", "Default");
  devices->SetString("video", "Default");
  DictionaryValue* content_settings = new DictionaryValue();
  content_settings->Set("media-stream", devices);
  DictionaryValue* pattern_pairs = new DictionaryValue();
  pattern_pairs->Set("https://*,*", content_settings);
  error = SetPreference(
      "profile.content_settings.pattern_pairs",
      true /* is_user_pref */,
      pattern_pairs);
  if (error)
    return error;
  const int kAllowContent = 1;
  DictionaryValue* default_content_settings = new DictionaryValue();
  default_content_settings->SetInteger("geolocation", kAllowContent);
  default_content_settings->SetInteger("mouselock", kAllowContent);
  default_content_settings->SetInteger("notifications", kAllowContent);
  default_content_settings->SetInteger("popups", kAllowContent);
  return SetPreference(
      "profile.default_content_settings",
      true /* is_user_pref */,
      default_content_settings);
}

Error* Session::SetPrefs() {
  DictionaryValue::key_iterator iter = capabilities_.prefs->begin_keys();
  for (; iter != capabilities_.prefs->end_keys(); ++iter) {
    Value* value;
    capabilities_.prefs->GetWithoutPathExpansion(*iter, &value);
    Error* error = SetPreference(*iter, true /* is_user_pref */,
                                 value->DeepCopy());
    if (error)
      return error;
  }
  iter = capabilities_.local_state->begin_keys();
  for (; iter != capabilities_.local_state->end_keys(); ++iter) {
    Value* value;
    capabilities_.local_state->GetWithoutPathExpansion(*iter, &value);
    Error* error = SetPreference(*iter, false /* is_user_pref */,
                                 value->DeepCopy());
    if (error)
      return error;
  }
  return NULL;
}

}  // namespace webdriver