#include "rootfstreemanager.h"

#include <fcntl.h>
#include <sys/statvfs.h>
#include <cstdio>

#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

#include "crypto/crypto.h"
#include "http/httpclient.h"
#include "ostree/repo.h"
#include "storage/invstorage.h"
#include "target.h"

const unsigned int RootfsTreeManager::Config::DefaultSysrootStorageWatermark{90};
const unsigned int RootfsTreeManager::Config::MinSysrootStorageWatermark{50};
const unsigned int RootfsTreeManager::Config::MaxSysrootStorageWatermark{95};

RootfsTreeManager::Config::Config(const PackageConfig& pconfig) {
  if (pconfig.extra.count(UpdateBlockParamName) == 1) {
    std::string val{pconfig.extra.at(UpdateBlockParamName)};
    UpdateBlock = val != "0" && val != "false";
  }
  if (pconfig.extra.count(SysrootStorageWatermarkParamName) == 1) {
    const std::string val_str{pconfig.extra.at(SysrootStorageWatermarkParamName)};
    try {
      const auto val{boost::lexical_cast<unsigned int>(val_str)};
      if (val < MinSysrootStorageWatermark) {
        LOG_ERROR << "Value of `" << SysrootStorageWatermarkParamName << "` parameter is too low: " << val_str
                  << "; setting it the minimum allowed: " << MinSysrootStorageWatermark;
        SysrootStorageWatermark = MinSysrootStorageWatermark;
      } else if (val > MaxSysrootStorageWatermark) {
        LOG_ERROR << "Value of `" << SysrootStorageWatermarkParamName << "` parameter is too high: " << val_str
                  << "; setting it the maximum allowed: " << MaxSysrootStorageWatermark;
        SysrootStorageWatermark = MaxSysrootStorageWatermark;
      } else {
        SysrootStorageWatermark = val;
      }
    } catch (const std::exception& exc) {
      LOG_ERROR << "Invalid value of `" << SysrootStorageWatermarkParamName << "` parameter: " << val_str
                << "; setting it the default value: " << DefaultSysrootStorageWatermark;
    }
  }
}

RootfsTreeManager::RootfsTreeManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                                     const std::shared_ptr<INvStorage>& storage,
                                     const std::shared_ptr<HttpInterface>& http,
                                     std::shared_ptr<OSTree::Sysroot> sysroot, const KeyManager& keys)
    : OstreeManager(pconfig, bconfig, storage, http, new bootloader::BootloaderLite(bconfig, *storage, sysroot)),
      sysroot_{std::move(sysroot)},
      boot_fw_update_status_{new bootloader::BootloaderLite(bconfig, *storage, sysroot_)},
      http_client_{http},
      gateway_url_{pconfig.ostree_server},
      keys_{keys},
      cfg_{pconfig} {}

DownloadResult RootfsTreeManager::Download(const TufTarget& target) {
  auto prog_cb = [this](const Uptane::Target& t, const std::string& description, unsigned int progress) {
    // report_progress_cb(events_channel.get(), t, description, progress);
    // TODO: consider make use of it for download progress reporting
  };

  std::vector<Remote> remotes = {{remote, config.ostree_server, {{"X-Correlation-ID", target.Name()}}, &keys_, false}};

  // Try to get additional remotes/origins to fetch an ostree commit from, unless
  // the base ostree server URL specified in a config refers not to http(s) server.
  // It helps to skip getting additional remotes if `ostree_server` refers to a local
  // ostree repo, i.e. file://<path to repo>
  if (!config.ostree_server.empty() && boost::starts_with(config.ostree_server, "http")) {
    getAdditionalRemotes(remotes, target.Name());
  }

  DownloadResult res{DownloadResult::Status::Ok, ""};
  data::InstallationResult pull_err{data::ResultCode::Numeric::kUnknown, ""};
  std::string error_desc;
  for (const auto& remote : remotes) {
    if (!remote.isRemoteSet) {
      setRemote(remote.name, remote.baseUrl, remote.keys);
    }

    DeltaStat delta_stat{};
    if (getDeltaStatIfAvailable(target, remote, delta_stat)) {
      LOG_INFO << "Found and pulled delta stats, checking if update can fit on a disk...";
      try {
        UpdateStat update_stat{};
        bool ok{canDeltaFitOnDisk(delta_stat, update_stat)};
        const auto stat_msg{boost::format{"required %u, available %u out of %u(%u%% of the volume capacity %u)"} %
                            update_stat.deltaSize % update_stat.available % update_stat.maxAvailable %
                            update_stat.highWatermark % update_stat.storageCapacity};

        if (!ok) {
          return {DownloadResult::Status::DownloadFailed_NoSpace,
                  "Insufficient storage available; err: " + stat_msg.str(), sysroot_->path()};
        }
        LOG_INFO << "Fetching static delta; " + stat_msg.str();
      } catch (const std::exception& exc) {
        LOG_ERROR << "Failed to check if the static delta can fit on a disk, skipping the update size check...; err: "
                  << exc.what();
        LOG_INFO << "Fetching ostree commit " + target.Sha256Hash() + " from " + remote.baseUrl;
      }
    } else {
      LOG_INFO << "No static delta or static delta stats are found, skipping the update size check...";
      LOG_INFO << "Fetching ostree commit " + target.Sha256Hash() + " from " + remote.baseUrl;
    }

    pull_err = OstreeManager::pull(config.sysroot, remote.baseUrl, keys_, Target::fromTufTarget(target), nullptr,
                                   prog_cb, remote.isRemoteSet ? nullptr : remote.name.c_str(), remote.headers);
    if (pull_err.isSuccess()) {
      res = {DownloadResult::Status::Ok, ""};
      break;
    }

    LOG_ERROR << "Failed to fetch from " + remote.baseUrl + ", err: " + pull_err.description;

    if (  // not enough storage space in the case of a regular pull (pulling objects/files)
        (pull_err.description.find("would be exceeded, at least") != std::string::npos &&
         (pull_err.description.find("min-free-space-size") != std::string::npos ||
          pull_err.description.find("min-free-space-percent") != std::string::npos)) ||
        // not enough storage space in the case of a static delta pull (pulling the delta parts/files)
        (pull_err.description.find("Delta requires") != std::string::npos &&
         pull_err.description.find("free space, but only") != std::string::npos)) {
      res = {DownloadResult::Status::DownloadFailed_NoSpace,
             "Insufficient storage available; path: " + config.sysroot.string() + "; err: " + pull_err.description,
             sysroot_->path()};
      break;
    }
    error_desc += pull_err.description + "\n";
    res = {DownloadResult::Status::DownloadFailed, error_desc};
  }

  return res;
}

bool RootfsTreeManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  (void)target;
  (void)fetcher;
  (void)token;
  (void)progress_cb;
  (void)keys;

  throw std::runtime_error("Using obsolete method of package manager: fetchTarget()");
}

void RootfsTreeManager::setInitialTargetIfNeeded(const std::string& hw_id) {
  const auto current{getCurrent()};
  if (!Target::isUnknown(current)) {
    return;
  }
  try {
    // Turning "unknown" Target to "initial" one
    Uptane::Target init_target{Target::toInitial(current, hw_id)};
    completeInitialTarget(init_target);
    storage_->savePrimaryInstalledVersion(init_target, InstalledVersionUpdateMode::kCurrent);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to set the initial Target: " << exc.what();
  }
}

void RootfsTreeManager::installNotify(const Uptane::Target& target) {
  if (sysroot_->reload()) {
    LOG_DEBUG << "Change in the ostree-based sysroot has been detected after installation;"
              << " booted on: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kCurrent)
              << " pending: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending);

  } else {
    LOG_WARNING << "Change in the ostree-based sysroot has NOT been detected after installation;"
                << " booted on: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kCurrent)
                << " pending: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending);
  }
  OstreeManager::installNotify(target);
}

data::InstallationResult RootfsTreeManager::install(const Uptane::Target& target) const {
  data::InstallationResult res;
  Uptane::Target current = OstreeManager::getCurrent();
  if (current.sha256Hash() != target.sha256Hash() && boot_fw_update_status_->isUpdateSupported()) {
    res = verifyBootloaderUpdate(target);
    if (res.result_code.num_code != data::ResultCode::Numeric::kOk) {
      return res;
    }
  }
  // Do ostree install if the currently installed target's hash differs from the specified target's hash,
  // or there is pending installation and it differs from the specified target so we undeploy it and make the new target
  // pending (app driven rollback)
  if ((current.sha256Hash() != target.sha256Hash()) ||
      (!sysroot()->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty() &&
       sysroot()->getDeploymentHash(OSTree::Sysroot::Deployment::kPending) != target.sha256Hash())) {
    // notify the bootloader before installation happens as it is not atomic
    // and a false notification doesn't hurt with rollback support in place
    // Hacking in order to invoke non-const method from the const one !!!
    const_cast<RootfsTreeManager*>(this)->updateNotify();
    res = OstreeManager::install(target);
    if (res.result_code.num_code == data::ResultCode::Numeric::kInstallFailed) {
      LOG_ERROR << "Failed to install OSTree target";
      return res;
    }
    const_cast<RootfsTreeManager*>(this)->installNotify(target);
    if (current.sha256Hash() == target.sha256Hash() &&
        res.result_code.num_code == data::ResultCode::Numeric::kNeedCompletion) {
      LOG_INFO << "Successfully undeployed the pending failing Target";
      LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
      const_cast<RootfsTreeManager*>(this)->updateNotify();
      res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
    }
  } else {
    LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
    res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
  }

  return res;
}

void RootfsTreeManager::getAdditionalRemotes(std::vector<Remote>& remotes, const std::string& target_name) {
  const auto resp = http_client_->post(gateway_url_ + "/download-urls", Json::Value::null);

  if (!resp.isOk()) {
    LOG_WARNING << "Failed to obtain download URLs from Gateway, fallback to download via gateway/proxy server: "
                << resp.getStatusStr();
    return;
  }

  const auto respJson = resp.getJson();
  for (Json::ValueConstIterator it = respJson.begin(); it != respJson.end(); it++) {
    remotes.emplace(
        remotes.begin(), Remote{"gcs",
                                (*it)["download_url"].asString(),
                                {
                                    {"X-Correlation-ID", target_name},
                                    {"Authorization", std::string("Bearer ") + (*it)["access_token"].asString()},
                                },
                                boost::none}

    );
  }
}

void RootfsTreeManager::setRemote(const std::string& name, const std::string& url,
                                  const boost::optional<const KeyManager*>& keys) {
  OSTree::Repo repo{sysroot_->path() + "/ostree/repo"};

  if (!!keys) {
    repo.addRemote(name, url, (*keys)->getCaFile(), (*keys)->getCertFile(), (*keys)->getPkeyFile());
  } else {
    repo.addRemote(name, url, "", "", "");
  }
}

data::InstallationResult RootfsTreeManager::verifyBootloaderUpdate(const Uptane::Target& target) const {
  if (cfg_.UpdateBlock && boot_fw_update_status_->isUpdateInProgress()) {
    LOG_WARNING << "Bootlader update is in progress."
                   " A device must be rebooted to confirm and finalize the boot fw update"
                   " before installation of a new Target with ostree/rootfs change";
    return data::InstallationResult(data::ResultCode::Numeric::kNeedCompletion, "bootlader update is in progress");
  }

  if (!boot_fw_update_status_->isRollbackProtectionEnabled()) {
    return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
  }

  std::string target_ver_str;
  try {
    target_ver_str = boot_fw_update_status_->getTargetVersion(target.sha256Hash());
  } catch (const std::invalid_argument& exc) {
    // Failure to parse the version file
    LOG_WARNING << "Rejecting the update because a bootloader version file is malformed: " << exc.what();
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, exc.what());
  } catch (const std::exception& exc) {
    LOG_INFO << "Failed to get bootloader version, assuming no bootloader update: " << exc.what();
    return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
  }

  uint64_t target_ver;
  try {
    target_ver = boost::lexical_cast<uint64_t>(target_ver_str);
  } catch (const std::exception& exc) {
    const std::string err_msg{"Invalid format of the bootloader version; value: " + target_ver_str +
                              "; err: " + exc.what()};
    LOG_ERROR << "Rejecting the update since the bootloader version has an invalid format; " << err_msg;
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, exc.what());
  }

  uint64_t cur_ver{0};
  std::string cur_ver_str;
  bool is_current_ver_valid;
  std::tie(cur_ver_str, is_current_ver_valid) = boot_fw_update_status_->getCurrentVersion();
  if (!is_current_ver_valid) {
    LOG_WARNING << "Failed to get current bootloader version: " << cur_ver_str;
    LOG_WARNING << "Assuming that the current bootloader version is `0` and proceeding with the update further";
    cur_ver_str = "0";
  }
  try {
    cur_ver = boost::lexical_cast<uint64_t>(cur_ver_str);
  } catch (const std::exception& exc) {
    LOG_WARNING << "Invalid format of the current bootloader version; value: " << cur_ver_str
                << "; err: " << exc.what();
    LOG_WARNING << "Assuming that the current bootloader version is `0` and proceeding with the update further";
  }

  if (target_ver < cur_ver) {
    const std::string err_msg{"bootloader rollback from version " + cur_ver_str + " to " + target_ver_str +
                              " has been detected"};
    LOG_WARNING << "Rejecting the update because " << err_msg;
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, err_msg);
  }

  return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
}

bool RootfsTreeManager::getDeltaStatIfAvailable(const TufTarget& target, const Remote& remote,
                                                DeltaStat& delta_stat) const {
  try {
    DeltaStatsRef delta_stats_ref;
    if (!getDeltaStatsRef(target.Custom(), delta_stats_ref)) {
      LOG_INFO << "No reference to static delta stats found in Target";
      return false;
    }
    LOG_INFO << "Found reference to a file with static delta stats, downloading it...";
    Json::Value delta_stats_json{downloadDeltaStats(delta_stats_ref, remote)};
    if (delta_stats_json.isNull()) {
      return false;
    }
    LOG_INFO << "File with static delta stats has been downloaded, parsing it...";
    if (!findDeltaStatForUpdate(delta_stats_json, getCurrentHash(), target.Sha256Hash(), delta_stat)) {
      LOG_ERROR << "No delta stat found between " << getCurrentHash() << " and " << target.Sha256Hash();
      return false;
    }
    return true;
  } catch (const std::exception& exc) {
    LOG_ERROR << "Error occurred while getting static delta stats: " << exc.what();
  }
  return false;
}

bool RootfsTreeManager::canDeltaFitOnDisk(const DeltaStat& delta_stat, UpdateStat& update_stat) const {
  StorageStat storage{};
  getStorageStat(sysroot_->path(), storage);
  const auto highWatermark{getStorageHighWatermark()};

  const uint64_t max_blocks_available = std::floor(storage.blockNumb * (static_cast<double>(highWatermark) / 100));
  const uint64_t blocks_in_use = storage.blockNumb - storage.freeBlockNumb;
  const uint64_t max_blocks_available_for_update =
      (max_blocks_available > blocks_in_use) ? (max_blocks_available - blocks_in_use) : 0;
  const uint64_t blocks_required_by_delta = (delta_stat.uncompressedSize / storage.blockSize) +
                                            ((delta_stat.uncompressedSize % storage.blockSize != 0) ? 1 : 0);

  update_stat.storageCapacity = storage.blockSize * storage.blockNumb;
  update_stat.highWatermark = highWatermark;
  update_stat.maxAvailable = max_blocks_available * storage.blockSize;
  update_stat.available = max_blocks_available_for_update * storage.blockSize;
  update_stat.deltaSize = delta_stat.uncompressedSize;

  return blocks_required_by_delta <= max_blocks_available_for_update;
}

bool RootfsTreeManager::getDeltaStatsRef(const Json::Value& json, DeltaStatsRef& ref) {
  if (!json.isMember("delta-stats")) {
    return false;
  }
  const auto delta_stats_ref{json["delta-stats"]};
  if (!delta_stats_ref.isMember("sha256") || !delta_stats_ref["sha256"].isString()) {
    LOG_ERROR << "Incorrect metadata about static delta statistics are found in Target;"
              << " err: missing `sha256` field or it's not a string";
    return false;
  }
  if (!delta_stats_ref.isMember("size") || !delta_stats_ref["size"].isUInt()) {
    LOG_ERROR << "Incorrect metadata about static delta statistics are found in Target;"
              << " err: missing `size` field or it's not an integer";
    return false;
  }
  ref = {delta_stats_ref["sha256"].asString(), delta_stats_ref["size"].asUInt()};
  return true;
}

Json::Value RootfsTreeManager::downloadDeltaStats(const DeltaStatsRef& ref, const Remote& remote) {
  static const uint64_t DeltaStatsMaxSize{1024 * 1024};
  const std::string uri = remote.baseUrl + "/delta-stats/" + ref.sha256;

  if (ref.size > DeltaStatsMaxSize) {
    LOG_ERROR << "Requested delta stat file has higher size than maximum allowed; "
                 " requested size: "
              << ref.size << ", maximum allowed: " << DeltaStatsMaxSize;
    return Json::nullValue;
  }
  std::vector<std::string> extra_headers;
  for (const auto& header : remote.headers) {
    extra_headers.emplace_back(header.first + ": " + header.second);
  }
  auto client{HttpClient(&extra_headers)};

  LOG_INFO << "Fetching delta stats -> " << uri;
  const auto resp = client.get(uri, ref.size);
  if (!resp.isOk()) {
    LOG_ERROR << "Failed to fetch static delta stats; status: " << resp.getStatusStr() << ", err: " << resp.body;
    return Json::nullValue;
  }
  if (resp.body.size() != ref.size) {
    LOG_ERROR << "Fetched invalid static delta stats, size mismatch; "
              << " expected: " << ref.size << ", got: " << resp.body.size();
    return Json::nullValue;
  }
  const auto received_data_hash{
      boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(resp.body)))};
  if (received_data_hash != ref.sha256) {
    LOG_ERROR << "Fetched invalid static delta stats, hash mismatch; "
              << " expected: " << ref.sha256 << ", got: " << received_data_hash;
    return Json::nullValue;
  }
  return resp.getJson();
}

bool RootfsTreeManager::findDeltaStatForUpdate(const Json::Value& delta_stats, const std::string& from,
                                               const std::string& to, DeltaStat& found_delta_stat) {
  if (!delta_stats.isMember(to)) {
    LOG_ERROR << "Invalid delta stats received; no `to` hash is found: " << to;
    return false;
  }
  const auto& to_json{delta_stats[to]};
  Json::Value found_delta;
  for (Json::ValueConstIterator it = to_json.begin(); it != to_json.end(); ++it) {
    if (it.key().asString() == from) {
      found_delta = *it;
      break;
    }
  }
  if (!found_delta) {
    return false;
  }
  if (!found_delta.isMember("size") || !found_delta["size"].isUInt64()) {
    LOG_ERROR << "Invalid delta stat has been found; `size` field is missing or is not `uint64`, " << found_delta;
    return false;
  }
  if (!found_delta.isMember("u_size") || !found_delta["u_size"].isUInt64()) {
    LOG_ERROR << "Invalid delta stat has been found; `u_size` field is missing or is not `uint64`, " << found_delta;
    return false;
  }
  found_delta_stat = {found_delta["size"].asUInt64(), found_delta["u_size"].asUInt64()};
  return true;
}

void RootfsTreeManager::getStorageStat(const std::string& path, StorageStat& stat_out) {
  int fd{-1};
  if ((fd = open(path.c_str(), O_DIRECTORY | O_RDONLY)) == -1) {
    throw std::runtime_error(std::string("Failed to obtain a sysroot directory file descriptor; path: ") + path +
                             ", err: " + std::strerror(errno));
  }
  struct statvfs fs_stat {};
  if (-1 == fstatvfs(fd, &fs_stat)) {
    throw std::runtime_error(std::string("Failed to obtain statistic about the sysroot directory; path: ") + path +
                             ", err: " + std::strerror(errno));
  }

  stat_out.freeBlockNumb = (getuid() != 0 ? fs_stat.f_bavail : fs_stat.f_bfree);
  stat_out.blockNumb = fs_stat.f_blocks;
  stat_out.blockSize = fs_stat.f_bsize;  // f_frsize == f_bsize on the linux-based systems
}
