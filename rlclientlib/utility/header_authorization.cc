#include "header_authorization.h"
#include <codecvt>

using namespace utility;
using convert_t = std::codecvt_utf8<wchar_t>;

namespace reinforcement_learning {
  int header_authorization::init(const utility::configuration& config, api_status* status, i_trace* trace) {
    const auto api_key = config.get(name::HTTP_API_KEY, nullptr);
    if (api_key == nullptr) {
      RETURN_ERROR(trace, status, http_api_key_not_provided);
    }
    _api_key = api_key;
    #ifdef _WIN32
    std::wstring_convert<convert_t, wchar_t> strconverter;
    _http_api_header_key_name = strconverter.from_bytes(config.get(name::HTTP_API_HEADER_KEY_NAME, value::HTTP_API_DEFAULT_HEADER_KEY_NAME));
    #endif
    #ifndef _WIN32
    _http_api_header_key_name = config.get(name::HTTP_API_HEADER_KEY_NAME, value::HTTP_API_DEFAULT_HEADER_KEY_NAME);
    #endif
    //_http_api_header_key_name = boost::locale::conv::utf_to_utf<char_t>(config.get(name::HTTP_API_HEADER_KEY_NAME, value::HTTP_API_DEFAULT_HEADER_KEY_NAME));
    return error_code::success;
  }

  int header_authorization::get_http_headers(http_headers& headers, api_status* status) {
    headers.add(_http_api_header_key_name, _api_key.c_str());
    return error_code::success;
  }
}
