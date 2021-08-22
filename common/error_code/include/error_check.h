#ifndef __ERROR_CHECK_H__
#define __ERROR_CHECK_H__

#include <glog/logging.h>

#define FFVMS_SUCC std::error_code{}

inline std::error_code ErrorFromErrno(int error_errno) { return std::make_error_code(static_cast<std::errc>(error_errno));}

/**
 * 日志输出到标准输出
 */
#define PRINT_ERROR_INFO(error_name) fprintf(stderr, "\033[31m%s(%i) in %s(): runtime error %d\n\033[0m", __FILE__, __LINE__, __FUNCTION__, error_name)
#define PRINT_WARNING_INFO(error_name) fprintf(stderr, "\033[33m%s(%i) in %s(): runtime warning %d\n\033[0m", __FILE__, __LINE__, __FUNCTION__, error_name)

#define CHECK_RTN_OF_FUNC(f) for (int _rtn_ = f; _rtn_;) return _rtn_
#define CHECK_RTN_DESC(x, y) if (!x) ; else for ((PRINT_ERROR_INFO(x.value())), (fprintf(stderr, "%s\n", y)); ;) return x
#define CHECK_RTN_DESC_OF_FUNC(f, y) for (int _rtn_ = f; _rtn_; ) for ((PRINT_ERROR_INFO(_rtn_.value())), (fprintf(stderr, "%s\n", y)); ; ) return _rtn_
#define CHECK_RTN_WARNING(x) if (!x); else PRINT_WARNING_INFO(x.value())
#define CHECK_RTN_WARNING_OF_FUNC(f) for (int _rtn_ = f; _rtn_; ) PRINT_WARNING_INFO(_rtn_.value())

/**
 * 日志通过glog输出
 */

inline std::string PrintReadableRTN(const std::error_code errc) { return errc.message(); }

#define CHECK_RTN_LOGW(x) if (!x); else for ((LOG(WARNING) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl);;) return x
#define CHECK_RTN_LOGE(x) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl);;) return x
#define CHECK_RTN_LOGF(x) if (!x); else for ((LOG(FATAL) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl);;)
#define CHECK_RTN_LOGW_DESC(x, y) if (!x); else for ((LOG(WARNING) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl);;) return x
#define CHECK_RTN_LOGE_DESC(x, y) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl);;) return x
#define CHECK_RTN_LOGF_DESC(x, y) if (!x); else for ((LOG(FATAL) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl);;)
#define CHECK_RTN_OF_FUNC_LOGW(f) for (error_code _rtn_ = f(); _rtn_; ) for ((LOG(WARNING) << "[ERROR CODE]: " << PrintReadableRTN(_rtn_) << ", " << std::endl);;) return _rtn_
#define CHECK_RTN_OF_FUNC_LOGE(f) for (error_code _rtn_ = f(); _rtn_; ) for ((LOG(ERROR) << "[ERROR CODE]: " << PrintReadableRTN(_rtn_) << ", " << std::endl);;) return _rtn_
#define CHECK_RTN_OF_FUNC_LOGF(f) for (error_code _rtn_ = f(); _rtn_; ) for ((LOG(FATAL) << "[ERROR CODE]: " << PrintReadableRTN(_rtn_) << ", " << std::endl);;) return _rtn_
#define CHECK_RTN_OF_FUNC_LOGW_DESC(f, y) for (error_code _rtn_ = f(); _rtn_; ) for ((LOG(WARNING) << "[ERROR CODE]: " << PrintReadableRTN(_rtn_) << ", " << y << std::endl);;) return _rtn_
#define CHECK_RTN_OF_FUNC_LOGE_DESC(f, y) for (error_code _rtn_ = f(); _rtn_; ) for ((LOG(ERROR) << "[ERROR CODE]: " << PrintReadableRTN(_rtn_) << ", " << y << std::endl);;) return _rtn_
#define CHECK_RTN_OF_FUNC_LOGF_DESC(f, y) for (error_code _rtn_ = f(); _rtn_; ) for ((LOG(FATAL) << "[ERROR CODE]: " << PrintReadableRTN(_rtn_) << ", " << y << std::endl);;) return _rtn_

#define CHECK_RTN_LOGE_RETURN_VOID(x) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl);;) return
#define CHECK_RTN_LOGE_DESC_RETURN_VOID(x, y) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl);;) return

#define CHECK_RTN_LOGE_DESC_RETURN(x, y, z) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl);;) return z

#define PRINT_RTN_LOGE(x) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl);;) break
#define PRINT_RTN_LOGE_DESC(x, y) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl);;) break
#define PRINT_RTN_LOGE_OF_FUNC(f) for (int x = f; x; ) { for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl); false;); break; }

#define CHECK_RTN_LOGE_CTN(x) if (!x); else {LOG(ERROR) << __PRETTY_FUNCTION__ << "[ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl; continue;}
#define CHECK_RTN_LOGE_CTN_DESC(x, y) if (!x); else {LOG(ERROR) << __PRETTY_FUNCTION__ << "[ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl; continue;}
#define CHECK_RTN_LOGE_BRK(x) if (!x); else {LOG(ERROR) << __PRETTY_FUNCTION__ << "[ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << std::endl; break;}
#define CHECK_RTN_LOGE_BRK_DESC(x, y) if (!x); else {LOG(ERROR) << __PRETTY_FUNCTION__ << "[ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << y << std::endl; break;}


// #define CHECK_GRPC_RTN_LOGE_DESC(x, y, z) if (!x); else for ((LOG(ERROR) << __PRETTY_FUNCTION__ << ": [ERROR CODE]: " << x << ", " << PrintReadableRTN(x) << ", " << z << std::endl);;) return grpc::Status(y, z);

#endif // __ERROR_CHECK_H__