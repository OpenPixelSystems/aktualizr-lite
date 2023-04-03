#ifndef AKTUALIZR_LITE_BOOTLOADERLITE_H_
#define AKTUALIZR_LITE_BOOTLOADERLITE_H_

#include "bootloader/bootloader.h"
#include "libaktualizr/config.h"
#include "ostree/sysroot.h"

class INvStorage;

namespace bootloader {

class BootFwUpdateStatus {
 public:
  BootFwUpdateStatus(const BootFwUpdateStatus&) = delete;
  BootFwUpdateStatus& operator=(const BootFwUpdateStatus&) = delete;
  BootFwUpdateStatus& operator=(BootFwUpdateStatus&&) = delete;
  virtual ~BootFwUpdateStatus() = default;

  virtual bool isUpdateInProgress() const = 0;
  virtual bool isUpdateSupported() const = 0;

 protected:
  BootFwUpdateStatus() = default;
  BootFwUpdateStatus(BootFwUpdateStatus&&) = default;
};

class BootloaderLite : public Bootloader, public BootFwUpdateStatus {
 public:
  using VersionType = uint64_t;  // clang-tidy prefers uint64_t over unsigned long long int
  using VersionNumbRes = std::tuple<VersionType, bool>;

  static constexpr const char* const VersionFile{"/usr/lib/firmware/version.txt"};

  explicit BootloaderLite(BootloaderConfig config, INvStorage& storage, OSTree::Sysroot::Ptr sysroot);

  VersionNumbRes getDeploymentVersion(const std::string& hash) const;
  VersionNumbRes getCurrentVersion() const;

  static std::string getVersion(const std::string& deployment_dir, const std::string& hash,
                                const std::string& ver_file = VersionFile);

  void installNotify(const Uptane::Target& target) const override;

  bool isUpdateInProgress() const override;
  bool isUpdateSupported() const override { return !get_env_cmd_.empty(); }

 private:
  bool isRollbackProtectionEnabled() const;
  std::tuple<std::string, bool> setEnvVar(const std::string& var_name, const std::string& var_val) const;
  std::tuple<std::string, bool> getEnvVar(const std::string& var_name) const;

  static VersionNumbRes verStrToNumber(const std::string& ver_str);
  static std::string extractVersionValue(const std::string& version_line);

  OSTree::Sysroot::Ptr sysroot_;
  const std::string get_env_cmd_;
  const std::string set_env_cmd_;
};

}  // namespace bootloader

#endif  // AKTUALIZR_LITE_BOOTLOADERLITE_H_
