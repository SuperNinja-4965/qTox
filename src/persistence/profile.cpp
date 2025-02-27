/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2015-2019 by The qTox Project Contributors
 * Copyright © 2024-2025 The TokTok team.
 */

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QObject>
#include <QSaveFile>
#include <QThread>

#include <cassert>
#include <sodium.h>

#include "profile.h"
#include "profilelocker.h"
#include "settings.h"
#include "src/core/core.h"
#include "src/core/coreav.h"
#include "src/core/corefile.h"
#include "src/net/avatarbroadcaster.h"
#include "src/net/bootstrapnodeupdater.h"
#include "src/nexus.h"
#include "src/widget/tool/identicon.h"
#include "src/widget/tool/imessageboxmanager.h"
#include "src/widget/widget.h"

namespace {
enum class LoadToxDataError
{
    OK = 0,
    FILE_NOT_FOUND,
    COULD_NOT_READ_FILE,
    FILE_IS_EMPTY,
    ENCRYPTED_NO_PASSWORD,
    COULD_NOT_DERIVE_KEY,
    DECRYPTION_FAILED,
    DECRYPT_UNENCRYPTED_FILE
};

enum class CreateToxDataError
{
    OK = 0,
    COULD_NOT_DERIVE_KEY,
    PROFILE_LOCKED,
    ALREADY_EXISTS,
    LOCK_FAILED
};

/**
 * Loads tox data from a file.
 * @param password The password to use to unlock the tox file.
 * @param filePath The path to the tox save file.
 * @param data A QByteArray reference where data will be stored.
 * @param error A LoadToxDataError enum value indicating operation result.
 * @return Pointer to the tox encryption key.
 */
std::unique_ptr<ToxEncrypt> loadToxData(const QString& password, const QString& filePath,
                                        QByteArray& data, LoadToxDataError& error)
{
    std::unique_ptr<ToxEncrypt> tmpKey;
    qint64 fileSize = 0;

    QFile saveFile(filePath);
    qDebug() << "Loading tox save" << filePath;

    if (!saveFile.exists()) {
        error = LoadToxDataError::FILE_NOT_FOUND;
        return nullptr;
    }

    if (!saveFile.open(QIODevice::ReadOnly)) {
        error = LoadToxDataError::COULD_NOT_READ_FILE;
        return nullptr;
    }

    fileSize = saveFile.size();
    if (fileSize <= 0) {
        error = LoadToxDataError::FILE_IS_EMPTY;
        return nullptr;
    }

    data = saveFile.readAll();
    if (ToxEncrypt::isEncrypted(data)) {
        if (password.isEmpty()) {
            error = LoadToxDataError::ENCRYPTED_NO_PASSWORD;
            return nullptr;
        }

        tmpKey = ToxEncrypt::makeToxEncrypt(password, data);
        if (!tmpKey) {
            error = LoadToxDataError::COULD_NOT_DERIVE_KEY;
            return nullptr;
        }

        data = tmpKey->decrypt(data);
        if (data.isEmpty()) {
            error = LoadToxDataError::DECRYPTION_FAILED;
            return nullptr;
        }
    }

    error = LoadToxDataError::OK;
    return tmpKey;
}

/**
 * Create a new tox data save file.
 * @param name The name to use for the new data file.
 * @param password The password to encrypt the data file with, if any.
 * @param error A CreateToxDataError enum value indicating operation result.
 * @return Pointer to the tox encryption key.
 */
std::unique_ptr<ToxEncrypt> createToxData(const QString& name, const QString& password,
                                          const QString& filePath, CreateToxDataError& error,
                                          Paths& paths)
{
    std::unique_ptr<ToxEncrypt> newKey;
    if (!password.isEmpty()) {
        newKey = ToxEncrypt::makeToxEncrypt(password);
        if (!newKey) {
            error = CreateToxDataError::COULD_NOT_DERIVE_KEY;
            return nullptr;
        }
    }

    if (ProfileLocker::hasLock()) {
        error = CreateToxDataError::PROFILE_LOCKED;
        return nullptr;
    }

    if (QFile::exists(filePath)) {
        error = CreateToxDataError::ALREADY_EXISTS;
        return nullptr;
    }

    if (!ProfileLocker::lock(name, paths)) {
        error = CreateToxDataError::LOCK_FAILED;
        return nullptr;
    }

    error = CreateToxDataError::OK;
    return newKey;
}

bool logLoadToxDataError(const LoadToxDataError& error, const QString& path)
{
    switch (error) {
    case LoadToxDataError::OK:
        return false;
    case LoadToxDataError::FILE_NOT_FOUND:
        qWarning() << "The tox save file" << path << "was not found";
        break;
    case LoadToxDataError::COULD_NOT_READ_FILE:
        qCritical() << "The tox save file" << path << "couldn't be opened";
        break;
    case LoadToxDataError::FILE_IS_EMPTY:
        qWarning() << "The tox save file" << path << "is empty";
        break;
    case LoadToxDataError::ENCRYPTED_NO_PASSWORD:
        qCritical() << "The tox save file is encrypted, but we don't have a password";
        break;
    case LoadToxDataError::COULD_NOT_DERIVE_KEY:
        qCritical() << "Failed to derive key of the tox save file";
        break;
    case LoadToxDataError::DECRYPTION_FAILED:
        qCritical() << "Failed to decrypt the tox save file";
        break;
    case LoadToxDataError::DECRYPT_UNENCRYPTED_FILE:
        qWarning() << "We have a password, but the tox save file is not encrypted";
        break;
    default:
        break;
    }
    return true;
}

bool logCreateToxDataError(const CreateToxDataError& error, const QString& userName)
{
    switch (error) {
    case CreateToxDataError::OK:
        return false;
    case CreateToxDataError::COULD_NOT_DERIVE_KEY:
        qCritical() << "Failed to derive key for the tox save";
        break;
    case CreateToxDataError::PROFILE_LOCKED:
        qCritical().nospace() << "Tried to create profile " << userName
                              << ", but another profile is already locked";
        break;
    case CreateToxDataError::ALREADY_EXISTS:
        qCritical().nospace() << "Tried to create profile " << userName << ", but it already exists";
        break;
    case CreateToxDataError::LOCK_FAILED:
        qWarning() << "Failed to lock profile" << userName;
        break;
    default:
        break;
    }
    return true;
}
} // namespace

/**
 * @class Profile
 * @brief Manages user profiles.
 *
 * @var bool Profile::newProfile
 * @brief True if this is a newly created profile, with no .tox save file yet.
 *
 * @var bool Profile::isRemoved
 * @brief True if the profile has been removed by remove().
 */

QStringList Profile::profiles;

void Profile::initCore(const QByteArray& toxSave, Settings& s, bool isNewProfile,
                       CameraSource& cameraSource)
{
    if (toxSave.isEmpty() && !isNewProfile) {
        qCritical() << "Existing toxSave is empty";
        emit failedToStart();
    }

    if (!toxSave.isEmpty() && isNewProfile) {
        qCritical() << "New profile has toxSave data";
        emit failedToStart();
    }

    bootstrapNodes =
        std::unique_ptr<BootstrapNodeUpdater>(new BootstrapNodeUpdater(s.getProxy(), paths));

    Core::ToxCoreErrors err;
    core = Core::makeToxCore(toxSave, s, *bootstrapNodes, &err);
    if (!core) {
        switch (err) {
        case Core::ToxCoreErrors::BAD_PROXY:
            emit badProxy();
            break;
        case Core::ToxCoreErrors::ERROR_ALLOC:
        case Core::ToxCoreErrors::FAILED_TO_START:
        case Core::ToxCoreErrors::INVALID_SAVE:
        default:
            emit failedToStart();
        }

        qDebug() << "Failed to start Toxcore";
        return;
    }

    coreAv = CoreAV::makeCoreAV(core->getTox(), core->getCoreLoopLock(), s, s, cameraSource);
    if (!coreAv) {
        qDebug() << "Failed to start ToxAV";
        emit failedToStart();
        return;
    }

    // Tell Core that we run with AV before doing anything else
    core->setAv(coreAv.get());
    coreAv->start();

    if (isNewProfile) {
        core->setStatusMessage(tr("Toxing on qTox"));
        core->setUsername(name);
        onSaveToxSave();
    }

    // save tox file when Core requests it
    connect(core.get(), &Core::saveRequest, this, &Profile::onSaveToxSave);
    // react to avatar changes
    connect(core.get(), &Core::friendAvatarRemoved, this, &Profile::removeAvatar);
    connect(core.get(), &Core::friendAvatarChanged, this, &Profile::setFriendAvatar);
    connect(core.get(), &Core::fileAvatarOfferReceived, this, &Profile::onAvatarOfferReceived,
            Qt::ConnectionType::QueuedConnection);
    // broadcast our own avatar
    avatarBroadcaster = std::unique_ptr<AvatarBroadcaster>(new AvatarBroadcaster(*core));
}

Profile::Profile(const QString& name_, std::unique_ptr<ToxEncrypt> passkey_, Paths& paths_,
                 Settings& settings_)
    : name{name_}
    , passkey{std::move(passkey_)}
    , isRemoved{false}
    , encrypted{passkey != nullptr}
    , paths{paths_}
    , settings{settings_}
{
}

/**
 * @brief Locks and loads an existing profile and creates the associate Core* instance.
 * @param name Profile name.
 * @param password Profile password.
 * @return Returns a nullptr on error. Profile pointer otherwise.
 *
 * @note If the profile is already in use return nullptr.
 */
Profile* Profile::loadProfile(const QString& name, const QString& password, Settings& settings,
                              const QCommandLineParser* parser, CameraSource& cameraSource,
                              IMessageBoxManager& messageBoxManager)
{
    if (ProfileLocker::hasLock()) {
        qCritical().nospace() << "Tried to load profile " << name
                              << ", but another profile is already locked";
        return nullptr;
    }

    Paths& paths = settings.getPaths();
    if (!ProfileLocker::lock(name, paths)) {
        qWarning() << "Failed to lock profile" << name;
        return nullptr;
    }

    LoadToxDataError error;
    QByteArray toxSave = QByteArray();
    QString path = paths.getSettingsDirPath() + name + ".tox";
    std::unique_ptr<ToxEncrypt> tmpKey = loadToxData(password, path, toxSave, error);
    if (logLoadToxDataError(error, path)) {
        ProfileLocker::unlock();
        return nullptr;
    }

    Profile* p = new Profile(name, std::move(tmpKey), paths, settings);

    // Core settings are saved per profile, need to load them before starting Core
    constexpr bool isNewProfile = false;
    settings.updateProfileData(p, parser, isNewProfile);

    p->initCore(toxSave, settings, isNewProfile, cameraSource);
    p->loadDatabase(password, messageBoxManager);

    return p;
}

/**
 * @brief Creates a new profile and the associated Core* instance.
 * @param name Username.
 * @param password If password is not empty, the profile will be encrypted.
 * @return Returns a nullptr on error. Profile pointer otherwise.
 *
 * @note If the profile is already in use return nullptr.
 */
Profile* Profile::createProfile(const QString& name, const QString& password, Settings& settings,
                                const QCommandLineParser* parser, CameraSource& cameraSource,
                                IMessageBoxManager& messageBoxManager)
{
    CreateToxDataError error;
    Paths& paths = settings.getPaths();
    QString path = paths.getSettingsDirPath() + name + ".tox";
    std::unique_ptr<ToxEncrypt> tmpKey = createToxData(name, password, path, error, paths);

    if (logCreateToxDataError(error, name)) {
        return nullptr;
    }

    Settings::createPersonal(paths, name);
    Profile* p = new Profile(name, std::move(tmpKey), paths, settings);

    constexpr bool isNewProfile = true;
    settings.updateProfileData(p, parser, isNewProfile);

    p->initCore(QByteArray(), settings, isNewProfile, cameraSource);
    p->loadDatabase(password, messageBoxManager);
    return p;
}

void Profile::save()
{
    if (isRemoved) {
        return;
    }

    onSaveToxSave();
    settings.savePersonal();
    settings.sync();
    ProfileLocker::assertLock(paths);
    assert(ProfileLocker::getCurLockName() == name);
    ProfileLocker::unlock();
}

Profile::~Profile() = default;

/**
 * @brief Lists all the files in the config dir with a given extension
 * @param extension Raw extension, e.g. "jpeg" not ".jpeg".
 * @return Vector of filenames.
 */
QStringList Profile::getFilesByExt(QString extension, Paths& paths)
{
    QDir dir(paths.getSettingsDirPath());
    QStringList out;
    dir.setFilter(QDir::Files | QDir::NoDotAndDotDot);
    dir.setNameFilters(QStringList("*." + extension));
    QFileInfoList list = dir.entryInfoList();
    out.reserve(list.size());
    for (QFileInfo file : list) {
        out += file.completeBaseName();
    }

    return out;
}

/**
 * @brief Scan for profile, automatically importing them if needed.
 * @warning NOT thread-safe.
 */
const QStringList Profile::getAllProfileNames(Paths& paths)
{
    profiles.clear();
    QStringList toxFiles = getFilesByExt("tox", paths), iniFiles = getFilesByExt("ini", paths);
    for (const QString& toxFile : toxFiles) {
        if (!iniFiles.contains(toxFile)) {
            Settings::createPersonal(paths, toxFile);
        }

        profiles.append(toxFile);
    }
    return profiles;
}

Core& Profile::getCore() const
{
    Core* c = core.get();
    assert(c != nullptr);
    return *c;
}

QString Profile::getName() const
{
    return name;
}

/**
 * @brief Starts the Core thread
 */
void Profile::startCore()
{
    // kriby: code duplication belongs in initCore, but cannot yet due to Core/Profile coupling
    connect(core.get(), &Core::requestSent, this, &Profile::onRequestSent);
    emit coreChanged(*core);

    core->start();

    const ToxPk& selfPk = core->getSelfPublicKey();
    const QByteArray data = loadAvatarData(selfPk);
    if (data.isEmpty()) {
        qDebug() << "Self avatar not found, will broadcast empty avatar to friends";
    }
    // TODO(sudden6): moved here, because it crashes in the constructor
    // reason: Core::getInstance() returns nullptr, because it's not yet initialized
    // solution: kill Core::getInstance
    setAvatar(data);
}

/**
 * @brief Saves the profile's .tox save, encrypted if needed.
 * @warning Invalid on deleted profiles.
 */
void Profile::onSaveToxSave()
{
    QByteArray data = core->getToxSaveData();
    assert(data.size());
    saveToxSave(data);
}

// TODO(sudden6): handle this better maybe?
void Profile::onAvatarOfferReceived(uint32_t friendId, uint32_t fileId,
                                    const QByteArray& avatarHash, uint64_t filesize)
{
    // accept if we don't have it already
    const bool accept = getAvatarHash(core->getFriendPublicKey(friendId)) != avatarHash;
    core->getCoreFile()->handleAvatarOffer(friendId, fileId, accept, filesize);
}

/**
 * @brief Write the .tox save, encrypted if needed.
 * @param data Byte array of profile save.
 * @return true if successfully saved, false otherwise
 * @warning Invalid on deleted profiles.
 */
bool Profile::saveToxSave(QByteArray data)
{
    assert(!isRemoved);
    ProfileLocker::assertLock(paths);
    assert(ProfileLocker::getCurLockName() == name);

    QString path = paths.getSettingsDirPath() + name + ".tox";
    qDebug() << "Saving tox save to" << path;
    QSaveFile saveFile(path);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        qCritical() << "Tox save file" << path << "couldn't be opened";
        return false;
    }

    if (encrypted) {
        data = passkey->encrypt(data);
        if (data.isEmpty()) {
            qCritical() << "Failed to encrypt, can't save";
            saveFile.cancelWriting();
            return false;
        }
    }

    saveFile.write(data);

    // check if everything got written
    if (saveFile.flush()) {
        saveFile.commit();
    } else {
        saveFile.cancelWriting();
        qCritical() << "Failed to write, can't save";
        return false;
    }
    return true;
}

/**
 * @brief Gets the path of the avatar file cached by this profile and corresponding to this owner
 * ID.
 * @param owner Path to avatar of friend with this PK will returned.
 * @param forceUnencrypted If true, return the path to the plaintext file even if this is an
 * encrypted profile.
 * @return Path to the avatar.
 */
QString Profile::avatarPath(const ToxPk& owner, bool forceUnencrypted)
{
    const QString ownerStr = owner.toString();
    if (!encrypted || forceUnencrypted) {
        return paths.getSettingsDirPath() + "avatars/" + ownerStr + ".png";
    }

    QByteArray idData = ownerStr.toUtf8();
    QByteArray pubkeyData = core->getSelfPublicKey().getByteArray();
    const uint32_t hashSize = tox_public_key_size();
    Q_ASSERT_X(hashSize >= crypto_generichash_BYTES_MIN && hashSize <= crypto_generichash_BYTES_MAX,
               "avatarPath", "Hash size not supported by libsodium");
    Q_ASSERT_X(hashSize >= crypto_generichash_KEYBYTES_MIN && hashSize <= crypto_generichash_KEYBYTES_MAX,
               "avatarPath", "Key size not supported by libsodium");
    QByteArray hash(hashSize, 0);
    crypto_generichash(reinterpret_cast<uint8_t*>(hash.data()), hashSize,
                       reinterpret_cast<uint8_t*>(idData.data()), idData.size(),
                       reinterpret_cast<uint8_t*>(pubkeyData.data()), pubkeyData.size());
    return paths.getSettingsDirPath() + "avatars/" + QString::fromUtf8(hash.toHex()).toUpper() + ".png";
}

/**
 * @brief Get our avatar from cache.
 * @return Avatar as QPixmap.
 */
QPixmap Profile::loadAvatar()
{
    return loadAvatar(core->getSelfPublicKey());
}

/**
 * @brief Get a contact's avatar from cache.
 * @param owner Friend PK to load avatar.
 * @return Avatar as QPixmap.
 */
QPixmap Profile::loadAvatar(const ToxPk& owner)
{
    QPixmap pic;
    if (settings.getShowIdenticons()) {

        const QByteArray avatarData = loadAvatarData(owner);
        if (avatarData.isEmpty()) {
            pic = QPixmap::fromImage(Identicon(owner.getByteArray()).toImage(16));
        } else {
            pic.loadFromData(avatarData);
        }

    } else {
        pic.loadFromData(loadAvatarData(owner));
    }

    return pic;
}

/**
 * @brief Get a contact's avatar from cache.
 * @param owner Friend PK to load avatar.
 * @return Avatar as QByteArray.
 */
QByteArray Profile::loadAvatarData(const ToxPk& owner)
{
    QString path = avatarPath(owner);
    bool avatarEncrypted = encrypted;
    // If the encrypted avatar isn't found, try loading the unencrypted one for the same ID
    if (avatarEncrypted && !QFile::exists(path)) {
        avatarEncrypted = false;
        path = avatarPath(owner, true);
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QByteArray pic = file.readAll();
    if (avatarEncrypted && !pic.isEmpty()) {
        pic = passkey->decrypt(pic);
        if (pic.isEmpty()) {
            qWarning() << "Failed to decrypt avatar at" << path;
        }
    }

    return pic;
}

void Profile::loadDatabase(QString password, IMessageBoxManager& messageBoxManager)
{
    assert(core);

    if (isRemoved) {
        qDebug() << "Can't load database of removed profile";
        return;
    }

    QByteArray salt = core->getSelfPublicKey().getByteArray();
    if (salt.size() != TOX_PASS_SALT_LENGTH) {
        qWarning() << "Couldn't compute salt from public key" << name;
        messageBoxManager
            .showError(QObject::tr("Error"),
                       QObject::tr("qTox couldn't open your chat logs, they will be disabled."));
    }
    // At this point it's too early to load the personal settings (Nexus will do it), so we always
    // load
    // the history, and if it fails we can't change the setting now, but we keep a nullptr
    database = std::make_shared<RawDatabase>(getDbPath(name, settings.getPaths()), password, salt);
    if (database && database->isOpen()) {
        history.reset(new History(database, settings, messageBoxManager));
    } else {
        qWarning() << "Failed to open database for profile" << name;
        messageBoxManager
            .showError(QObject::tr("Error"),
                       QObject::tr("qTox couldn't open your chat logs, they will be disabled."));
    }
}

/**
 * @brief Sets our own avatar
 * @param pic Picture to use as avatar, if empty an Identicon will be used depending on settings
 */
void Profile::setAvatar(QByteArray pic)
{
    QPixmap pixmap;
    QByteArray avatarData;
    const ToxPk& selfPk = core->getSelfPublicKey();
    if (!pic.isEmpty()) {
        pixmap.loadFromData(pic);
        avatarData = pic;
    } else {
        if (settings.getShowIdenticons()) {
            const QImage identicon = Identicon(selfPk.getByteArray()).toImage(32);
            pixmap = QPixmap::fromImage(identicon);

        } else {
            pixmap.load(":/img/contact_dark.svg");
        }
    }

    saveAvatar(selfPk, avatarData);

    emit selfAvatarChanged(pixmap);
    avatarBroadcaster->setAvatar(avatarData);
    avatarBroadcaster->enableAutoBroadcast();
}


/**
 * @brief Sets a friends avatar
 * @param pic Picture to use as avatar, if empty an Identicon will be used depending on settings
 * @param owner pk of friend
 */
void Profile::setFriendAvatar(const ToxPk& owner, QByteArray pic)
{
    QPixmap pixmap;
    QByteArray avatarData;
    if (!pic.isEmpty()) {
        pixmap.loadFromData(pic);
        avatarData = pic;
        emit friendAvatarSet(owner, pixmap);
    } else if (settings.getShowIdenticons()) {
        const QImage identicon = Identicon(owner.getByteArray()).toImage(32);
        pixmap = QPixmap::fromImage(identicon);
        emit friendAvatarSet(owner, pixmap);
    } else {
        pixmap.load(":/img/contact_dark.svg");
        emit friendAvatarRemoved(owner);
    }
    friendAvatarChanged(owner, pixmap);
    saveAvatar(owner, avatarData);
}

/**
 * @brief Adds history message about friendship request attempt if history is enabled
 * @param friendPk Pk of a friend which request is destined to
 * @param message Friendship request message
 */
void Profile::onRequestSent(const ToxPk& friendPk, const QString& message)
{
    if (!isHistoryEnabled()) {
        return;
    }

    const QString inviteStr = Core::tr("/me offers friendship, \"%1\"").arg(message);
    const ToxPk selfPk = core->getSelfPublicKey();
    const QDateTime datetime = QDateTime::currentDateTime();
    const QString selfName = core->getUsername();
    history->addNewMessage(friendPk, inviteStr, selfPk, datetime, true, selfName);
}

/**
 * @brief Save an avatar to cache.
 * @param pic Picture to save.
 * @param owner PK of avatar owner.
 */
void Profile::saveAvatar(const ToxPk& owner, const QByteArray& avatar)
{
    const bool needEncrypt = encrypted && !avatar.isEmpty();
    const QByteArray& pic = needEncrypt ? passkey->encrypt(avatar) : avatar;

    QString path = avatarPath(owner);
    QDir(paths.getSettingsDirPath()).mkdir("avatars");
    if (pic.isEmpty()) {
        QFile::remove(path);
    } else {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) {
            qWarning() << "Tox avatar" << path << "couldn't be saved";
            return;
        }
        file.write(pic);
        file.commit();
    }
}

/**
 * @brief Get the tox hash of a cached avatar.
 * @param owner Friend PK to get hash.
 * @return Avatar tox hash.
 */
QByteArray Profile::getAvatarHash(const ToxPk& owner)
{
    QByteArray pic = loadAvatarData(owner);
    QByteArray avatarHash(tox_hash_length(), 0);
    tox_hash(reinterpret_cast<uint8_t*>(avatarHash.data()), reinterpret_cast<uint8_t*>(pic.data()),
             pic.size());
    return avatarHash;
}

/**
 * @brief Removes our own avatar.
 */
void Profile::removeSelfAvatar()
{
    removeAvatar(core->getSelfPublicKey());
}

/**
 * @brief Removes friend avatar.
 */
void Profile::removeFriendAvatar(const ToxPk& owner)
{
    removeAvatar(owner);
}

/**
 * @brief Checks that the history is enabled in the settings, and loaded successfully for this
 * profile.
 * @return True if enabled, false otherwise.
 */
bool Profile::isHistoryEnabled()
{
    return settings.getEnableLogging() && history;
}

/**
 * @brief Get chat history.
 * @return May return a nullptr if the history failed to load.
 */
History* Profile::getHistory()
{
    return history.get();
}

/**
 * @brief Removes a cached avatar.
 * @param owner Friend PK whose avatar to delete.
 */
void Profile::removeAvatar(const ToxPk& owner)
{
    QFile::remove(avatarPath(owner));
    if (owner == core->getSelfPublicKey()) {
        setAvatar({});
    } else {
        setFriendAvatar(owner, {});
    }
}

bool Profile::exists(QString name, Paths& paths)
{
    QString path = paths.getSettingsDirPath() + name;
    return QFile::exists(path + ".tox");
}

/**
 * @brief Checks, if profile has a password.
 * @return True if we have a password set (doesn't check the actual file on disk).
 */
bool Profile::isEncrypted() const
{
    return encrypted;
}

/**
 * @brief Checks if profile is encrypted.
 * @note Checks the actual file on disk.
 * @param name Profile name.
 * @return True if profile is encrypted, false otherwise.
 */
bool Profile::isEncrypted(QString name, Paths& paths)
{
    uint8_t data[TOX_PASS_ENCRYPTION_EXTRA_LENGTH] = {0};
    QString path = paths.getSettingsDirPath() + name + ".tox";
    QFile saveFile(path);
    if (!saveFile.open(QIODevice::ReadOnly)) {
        qWarning() << "Couldn't open tox save" << path;
        return false;
    }

    saveFile.read(reinterpret_cast<char*>(data), TOX_PASS_ENCRYPTION_EXTRA_LENGTH);

    return tox_is_data_encrypted(data);
}

/**
 * @brief Removes the profile permanently.
 * Updates the profiles vector.
 * @return Vector of filenames that could not be removed.
 * @warning It is invalid to call loadToxSave or saveToxSave on a deleted profile.
 */
QStringList Profile::remove()
{
    if (isRemoved) {
        qWarning() << "Profile" << name << "is already removed";
        return {};
    }
    isRemoved = true;

    qDebug() << "Removing profile" << name;
    for (int i = 0; i < profiles.size(); ++i) {
        if (profiles[i] == name) {
            profiles.removeAt(i);
            i--;
        }
    }
    QString path = paths.getSettingsDirPath() + name;
    ProfileLocker::unlock();

    QFile profileMain{path + ".tox"};
    QFile profileConfig{path + ".ini"};

    QStringList ret;

    if (!profileMain.remove() && profileMain.exists()) {
        ret.push_back(profileMain.fileName());
        qWarning() << "Could not remove file" << profileMain.fileName();
    }
    if (!profileConfig.remove() && profileConfig.exists()) {
        ret.push_back(profileConfig.fileName());
        qWarning() << "Could not remove file" << profileConfig.fileName();
    }

    QString dbPath = getDbPath(name, settings.getPaths());
    if (database && database->isOpen() && !database->remove() && QFile::exists(dbPath)) {
        ret.push_back(dbPath);
        qWarning() << "Could not remove file" << dbPath;
    }

    history.reset();
    database.reset();

    return ret;
}

/**
 * @brief Tries to rename the profile.
 * @param newName New name for the profile.
 * @return False on error, true otherwise.
 */
bool Profile::rename(QString newName)
{
    QString path = paths.getSettingsDirPath() + name, newPath = paths.getSettingsDirPath() + newName;

    if (!ProfileLocker::lock(newName, paths)) {
        return false;
    }

    QFile::rename(path + ".tox", newPath + ".tox");
    QFile::rename(path + ".ini", newPath + ".ini");
    if (database) {
        database->rename(newName);
    }

    bool resetAutorun = settings.getAutorun();
    settings.setAutorun(false);
    settings.setCurrentProfile(newName);
    if (resetAutorun) {
        settings.setAutorun(true); // fixes -p flag in autostart command line
    }

    name = newName;
    return true;
}

const ToxEncrypt* Profile::getPasskey() const
{
    return passkey.get();
}

/**
 * @brief Changes the encryption password and re-saves everything with it
 * @param newPassword Password for encryption, if empty profile will be decrypted.
 * @param oldPassword Supply previous password if already encrypted or empty QString if not yet
 * encrypted.
 * @return Empty QString on success or error message on failure.
 */
QString Profile::setPassword(const QString& newPassword)
{
    if (newPassword.isEmpty()) {
        // remove password
        encrypted = false;
    } else {
        std::unique_ptr<ToxEncrypt> newpasskey = ToxEncrypt::makeToxEncrypt(newPassword);
        if (!newpasskey) {
            qCritical()
                << "Failed to derive key from password, the profile won't use the new password";
            return tr(
                "Failed to derive key from password, the profile won't use the new password.");
        }
        // apply change
        passkey = std::move(newpasskey);
        encrypted = true;
    }

    // apply new encryption
    onSaveToxSave();

    bool dbSuccess = false;

    // TODO: ensure the database and the tox save file use the same password
    if (database) {
        dbSuccess = database->setPassword(newPassword);
    }

    QString error{};
    if (!dbSuccess) {
        error = tr("Couldn't change database password, it may be corrupted or use the old "
                   "password.");
    }

    QByteArray avatar = loadAvatarData(core->getSelfPublicKey());
    saveAvatar(core->getSelfPublicKey(), avatar);

    QVector<uint32_t> friendList = core->getFriendList();
    QVectorIterator<uint32_t> i(friendList);
    while (i.hasNext()) {
        const ToxPk friendPublicKey = core->getFriendPublicKey(i.next());
        saveAvatar(friendPublicKey, loadAvatarData(friendPublicKey));
    }
    return error;
}

/**
 * @brief Retrieves the path to the database file for a given profile.
 * @param profileName Profile name.
 * @return Path to database.
 */
QString Profile::getDbPath(const QString& profileName, Paths& paths)
{
    return paths.getSettingsDirPath() + profileName + ".db";
}
