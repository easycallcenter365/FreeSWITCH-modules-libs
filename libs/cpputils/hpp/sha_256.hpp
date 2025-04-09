#include <iomanip>
#include <iostream>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h> 
#include <sstream>
#include <string>
#include <openssl/hmac.h>
#include <vector>


namespace EasyCallCenter365
{
	namespace Encryption
	{
		class SHA256
		{  
			public:
			static std::string base64Encode(const unsigned char *input, int length)
			{
				BIO *bio = BIO_new(BIO_f_base64()); // 创建Base64 BIO
				BIO *mem = BIO_new(BIO_s_mem());	// 创建内存BIO
				bio = BIO_push(bio, mem);			// 连接Base64 BIO和内存BIO

				BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL); // 不添加换行符
				BIO_write(bio, input, length);				// 写入数据
				BIO_flush(bio);								// 刷新BIO

				BUF_MEM *bufferPtr;
				BIO_get_mem_ptr(bio, &bufferPtr); // 获取Base64数据

				std::string result(bufferPtr->data, bufferPtr->length); // 拷贝数据到字符串
				BIO_free_all(bio);										// 释放BIO资源
				return result;
			}  

			/*
			 * 使用 HMAC-SHA1 算法计算 HMAC  
			 * @param encryptKey: 密钥
			 * @param encryptText: 待加密文本
			 * @return: HMAC-SHA1 的 Base64 编码结果
			 * @note: 密钥和待加密文本必须为 UTF-8 编码
			 * @example: hmacSha1Base64("keyxxx", "textxxx")
			 * 
			 */
			static std::string hmacSha1Base64(const std::string &encryptText, const std::string &encryptKey)
			{
				unsigned int len = 0;
				// 使用 OpenSSL 进行 HMAC-SHA256 计算
				unsigned char *hmac_result = HMAC(EVP_sha1(), encryptKey.c_str(), static_cast<int>(encryptKey.length()),
											 reinterpret_cast<const unsigned char *>(encryptText.c_str()),
											 encryptText.length(), nullptr, &len);
 				return base64Encode(hmac_result, len);
			}

			static std::string sha256(const std::string &input)
			{
				EVP_MD_CTX *context = EVP_MD_CTX_new();
				if (!context) { throw std::runtime_error("Failed to create EVP_MD_CTX"); }

				if (1 != EVP_DigestInit_ex(context, EVP_sha256(), nullptr)) {
					EVP_MD_CTX_free(context);
					throw std::runtime_error("Failed to initialize digest context");
				}

				if (1 != EVP_DigestUpdate(context, input.c_str(), input.size())) {
					EVP_MD_CTX_free(context);
					throw std::runtime_error("Failed to update digest");
				}

				unsigned char hash[EVP_MAX_MD_SIZE];
				unsigned int lengthOfHash = 0;
				if (1 != EVP_DigestFinal_ex(context, hash, &lengthOfHash)) {
					EVP_MD_CTX_free(context);
					throw std::runtime_error("Failed to finalize digest");
				}

				EVP_MD_CTX_free(context);

				return base64Encode(hash, lengthOfHash);
			}

		};

	}; // namespace Encryption

}; // namespace Encryption

   /*
int main()
{
	std::string input = "YXBpX2tleT0ia2V5eHh4eHhCSDt1UI7NX995LkkRQxQ0BMQ6f2pYo1673493479";
	std::string output = sha256(input);
	std::cout << "SHA-256 Base64 hash of \"" << input << "\": " << output << std::endl;
	return 0;
}
*/