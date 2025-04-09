#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include <chrono>
#include <curl/curl.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>

/*
编译说明：(需要先安装rapidjson、libcurl4-openssl-dev、libssl-dev)

sudo apt-get install libcurl4-openssl-dev   # Ubuntu/Debian
sudo yum install libcurl-devel               # CentOS/RHEL

sudo apt-get install libssl-dev   # Ubuntu/Debian
sudo yum install openssl-devel    # CentOS/RHEL

 g++  -o accessToken -fPIC -g -O -ggdb -std=c++11 -Wall \
AccessToken.cpp  -lcurl   -lpthread   -lssl -lcrypto


*/

class AccessToken
{
	public:
	AccessToken(const std::string &accessKeyId, const std::string &accessKeySecret)
		: accessKeyId(accessKeyId), accessKeySecret(accessKeySecret), domain("nls-meta.cn-shanghai.aliyuncs.com"),
		  regionId("cn-shanghai"), version("2019-02-28"), action("CreateToken"), expireTime(0)
	{
	}

	void apply()
	{
		expireTime = 0;
		std::map<std::string, std::string> queryParamsMap = {{"AccessKeyId", accessKeyId},
															 {"Action", action},
															 {"Version", version},
															 {"RegionId", regionId},
															 {"Timestamp", getISO8601Time()},
															 {"Format", "JSON"},
															 {"SignatureMethod", "HMAC-SHA1"},
															 {"SignatureVersion", "1.0"},
															 {"SignatureNonce", getUniqueNonce()}};

		std::string queryString = canonicalizedQuery(queryParamsMap);
		std::string method = "GET";
		std::string urlPath = "/";
		std::string stringToSign = createStringToSign(method, urlPath, queryString);
		std::cout << "generate string to sign: " << stringToSign << std::endl;

		std::string signature = sign(stringToSign, accessKeySecret + "&");
		std::cout << "generate signature: " << signature << std::endl;

		std::string queryStringWithSign = "Signature=" + UrlEncode(signature) + "&" + queryString;

		processGETRequest(queryStringWithSign);
	}

	std::string getToken() const { return token; }
	long getExpireTime() const { return expireTime; }

	private:
	std::string accessKeyId;
	std::string accessKeySecret;
	std::string domain;
	std::string regionId;
	std::string version;
	std::string action;
	std::string token;
	long expireTime;

	std::string getISO8601Time() const
	{
		auto now = std::chrono::system_clock::now();
		auto in_time_t = std::chrono::system_clock::to_time_t(now);
		std::stringstream ss;
		ss << std::put_time(gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
		return ss.str();
	}

	std::string getUniqueNonce() const
	{
		std::random_device rd;
		std::uniform_int_distribution<int> dist(0, 15);
		std::stringstream uuid;
		for (int i = 0; i < 32; i++) { uuid << std::hex << dist(rd); }
		return uuid.str();
	}

	std::string UrlEncode(const std::string &src) const
	{
		CURL *curl = curl_easy_init();
		char *output = curl_easy_escape(curl, src.c_str(), src.size());
		std::string result(output);
		curl_free(output);
		curl_easy_cleanup(curl);
		return result;
	}

	std::string canonicalizedQuery(const std::map<std::string, std::string> &queryParamsMap) const
	{
		std::string canonicalizedQueryString;
		for (const auto &param : queryParamsMap) {
			canonicalizedQueryString += "&" + UrlEncode(param.first) + "=" + UrlEncode(param.second);
		}
		return canonicalizedQueryString.substr(1);
	}

	std::string createStringToSign(const std::string &method, const std::string &urlPath,
								   const std::string &queryString) const
	{
		return method + "&" + UrlEncode(urlPath) + "&" + UrlEncode(queryString);
	}

	std::string sign(const std::string &src, const std::string &secret) const
	{
		if (src.empty()) return std::string();

		unsigned char md[EVP_MAX_BLOCK_LENGTH] = {0};
		unsigned int mdLen = EVP_MAX_BLOCK_LENGTH;

		if (HMAC(EVP_sha1(), secret.c_str(), secret.size(), reinterpret_cast<const unsigned char *>(src.c_str()),
				 src.size(), md, &mdLen) == NULL) {
			return std::string();
		}

		char encodedData[100] = {0};
		EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encodedData), md, mdLen);
		return encodedData;
	}

	void processGETRequest(const std::string &queryString)
	{
		std::string url = "http://" + domain + "/?" + queryString;
		std::cout << "Request URL: " << url << std::endl;
		CURL *curl = curl_easy_init();
		if (curl) {
			curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
			curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
			std::string response;
			curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

			CURLcode res = curl_easy_perform(curl);
			std::cout << "Response: " << response << std::endl;
			if (res == CURLE_OK) {
				rapidjson::Document doc;
				doc.Parse(response.c_str());
				if (doc.HasMember("Token")) {
					const rapidjson::Value &tokenObj = doc["Token"];
					if (tokenObj.IsObject()) {
						token = tokenObj["Id"].GetString();
						expireTime = tokenObj["ExpireTime"].GetInt64();
					}
				}
			} else {
				std::cerr << "Failed to create the token: " << curl_easy_strerror(res) << std::endl;
			}
			curl_easy_cleanup(curl);
		}
	}

	static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
	{
		((std::string *)userp)->append((char *)contents, size * nmemb);
		return size * nmemb;
	}
};

int _main()
{
	// 配置您的 Access Key ID 和 Access Key Secret
	std::string accessKeyId = "Your accessKeyId";
	std::string accessKeySecret = "Your accessKeySecret";

	// 创建 AccessToken 实例
	AccessToken accessToken(accessKeyId, accessKeySecret);

	try {
		// 调用 apply() 方法生成访问令牌
		accessToken.apply();

		// 获取并打印生成的令牌和过期时间
		std::string token = accessToken.getToken();
		long expireTime = accessToken.getExpireTime();

		std::cout << "Token: " << token << std::endl;
		std::cout << "Expire Time: " << expireTime << std::endl;
	} catch (const std::exception &e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return 1;
	}

	return 0;
};