#include <curl/curl.h>
#include <string>


namespace EasyCallCenter365
{
namespace Uri
{
class UriUtils
{

	public:
	static std::string UrlEncode(const std::string &src) 
	{
		CURL *curl = curl_easy_init();
		char *output = curl_easy_escape(curl, src.c_str(), src.size());
		std::string result(output);
		curl_free(output);
		curl_easy_cleanup(curl);
		return result;
	}

	static std::string UrlDecode(const std::string &src)  
	{
		CURL *curl = curl_easy_init();
		int outlength = 0;
		char *output = curl_easy_unescape(curl, src.c_str(), src.size(), &outlength);
		std::string result(output);
		curl_free(output);
		curl_easy_cleanup(curl);
		return result;
	}
};

}; // namespace Uri

}; // namespace EasyCallCenter365