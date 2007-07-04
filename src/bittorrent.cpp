/*
 * Bittorrent Client using Qt4 and libtorrent.
 * Copyright (C) 2006  Christophe Dumez
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Contact : chris@qbittorrent.org
 */

#include <QDir>
#include <QTime>
#include <QString>

#include "bittorrent.h"
#include "misc.h"
#include "downloadThread.h"

#define ETAS_MAX_VALUES 8

// Main constructor
bittorrent::bittorrent(){
  // To avoid some exceptions
  fs::path::default_name_check(fs::no_check);
  // Supported preview extensions
  // XXX: might be incomplete
  supported_preview_extensions << "AVI" << "DIVX" << "MPG" << "MPEG" << "MP3" << "OGG" << "WMV" << "WMA" << "RMV" << "RMVB" << "ASF" << "MOV" << "WAV" << "MP2" << "SWF" << "AC3";
  // Creating bittorrent session
  s = new session(fingerprint("qB", VERSION_MAJOR, VERSION_MINOR, VERSION_BUGFIX, 0));
  // Set severity level of libtorrent session
  s->set_severity_level(alert::info);
  // DHT (Trackerless), disabled until told otherwise
  DHTEnabled = false;
  // Enabling metadata plugin
  s->add_extension(&create_metadata_plugin);
  connect(&timerAlerts, SIGNAL(timeout()), this, SLOT(readAlerts()));
  timerAlerts.start(3000);
  connect(&ETARefresher, SIGNAL(timeout()), this, SLOT(updateETAs()));
  ETARefresher.start(6000);
  // To download from urls
  downloader = new downloadThread(this);
  connect(downloader, SIGNAL(downloadFinished(const QString&, const QString&, int, const QString&)), this, SLOT(processDownloadedFile(const QString&, const QString&, int, const QString&)));
}

// Main destructor
bittorrent::~bittorrent(){
  disableDirectoryScanning();
  delete downloader;
  delete s;
}

void bittorrent::resumeUnfinishedTorrents(){
  // Resume unfinished torrents
  resumeUnfinished();
}

void bittorrent::updateETAs(){
  std::vector<torrent_handle> handles = s->get_torrents();
  for(unsigned int i=0; i<handles.size(); ++i){
    torrent_handle h = handles[i];
    if(h.is_valid()){
      QString hash = QString(misc::toString(h.info_hash()).c_str());
      QList<long> listEtas = ETAstats.value(hash, QList<long>());
      if(listEtas.size() == ETAS_MAX_VALUES){
          listEtas.removeFirst();
      }
      torrent_status torrentStatus = h.status();
      torrent_info ti = h.get_torrent_info();
      if(torrentStatus.download_payload_rate != 0){
        listEtas << (long)((ti.total_size()-torrentStatus.total_done)/(double)torrentStatus.download_payload_rate);
        ETAstats[hash] = listEtas;
        long moy = 0;
        long val;
        foreach(val, listEtas){
          moy += val;
        }
        ETAs[hash] = (long) ((double)moy/(double)listEtas.size());
      }
    }
  }
}

long bittorrent::getETA(QString hash) const{
  return ETAs.value(hash, -1);
}

// Return the torrent handle, given its hash
torrent_handle bittorrent::getTorrentHandle(const QString& hash) const{
  return s->find_torrent(misc::fromString<sha1_hash>((hash.toStdString())));
}

// Return true if the torrent corresponding to the
// hash is paused
bool bittorrent::isPaused(const QString& hash) const{
  torrent_handle h = s->find_torrent(misc::fromString<sha1_hash>((hash.toStdString())));
  if(!h.is_valid()){
    qDebug("/!\\ Error: Invalid handle");
    return true;
  }
  return h.is_paused();
}

// Delete a torrent from the session, given its hash
// permanent = true means that the torrent will be removed from the hard-drive too
void bittorrent::deleteTorrent(const QString& hash, bool permanent){
  torrent_handle h = s->find_torrent(misc::fromString<sha1_hash>((hash.toStdString())));
  if(!h.is_valid()){
    qDebug("/!\\ Error: Invalid handle");
    return;
  }
  QString savePath = QString::fromUtf8(h.save_path().string().c_str());
  QString fileName = QString(h.name().c_str());
  // Remove it from session
  s->remove_torrent(h);
  // Remove it from torrent backup directory
  QDir torrentBackup(misc::qBittorrentPath() + "BT_backup");
  torrentBackup.remove(hash+".torrent");
  torrentBackup.remove(hash+".fastresume");
  torrentBackup.remove(hash+".paused");
  torrentBackup.remove(hash+".incremental");
  torrentBackup.remove(hash+".priorities");
  torrentBackup.remove(hash+".savepath");
  torrentBackup.remove(hash+".trackers");
  // Remove it fro ETAs hash tables
  ETAstats.take(hash);
  ETAs.take(hash);
  if(permanent){
    // Remove from Hard drive
    qDebug("Removing this on hard drive: %s", qPrintable(savePath+QDir::separator()+fileName));
    // Deleting in a thread to avoid GUI freeze
    deleteThread *deleter = new deleteThread(savePath+QDir::separator()+fileName);
    connect(deleter, SIGNAL(deletionFinished(deleteThread*)), this, SLOT(cleanDeleter(deleteThread*)));
  }
}

// slot to destroy a deleteThread once it finished deletion
void bittorrent::cleanDeleter(deleteThread* deleter){
  qDebug("Deleting deleteThread because it finished deletion");
  delete deleter;
}

// Pause a running torrent
void bittorrent::pauseTorrent(const QString& hash){
  torrent_handle h = s->find_torrent(misc::fromString<sha1_hash>((hash.toStdString())));
  if(h.is_valid() && !h.is_paused()){
    h.pause();
    // Create .paused file
    QFile paused_file(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+hash+".paused");
    paused_file.open(QIODevice::WriteOnly | QIODevice::Text);
    paused_file.close();
    int index = torrentsToPauseAfterChecking.indexOf(hash);
    if(index != -1) {
      torrentsToPauseAfterChecking.removeAt(index);
      qDebug("A torrent was paused just after checking, good");
    }
  }
}

// Resume a torrent in paused state
void bittorrent::resumeTorrent(const QString& hash){
  torrent_handle h = s->find_torrent(misc::fromString<sha1_hash>((hash.toStdString())));
  if(h.is_valid() && h.is_paused()){
    h.resume();
    // Delete .paused file
    QFile::remove(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+hash+".paused");
  }
}

// Add a torrent to the bittorrent session
void bittorrent::addTorrent(const QString& path, bool fromScanDir, bool onStartup, const QString& from_url){
  torrent_handle h;
  entry resume_data;
  bool fastResume=false;
  QDir torrentBackup(misc::qBittorrentPath() + "BT_backup");
  QString file, dest_file, scan_dir;

  // Checking if BT_backup Dir exists
  // create it if it is not
  if(! torrentBackup.exists()){
    if(! torrentBackup.mkpath(torrentBackup.path())){
      std::cerr << "Couldn't create the directory: '" << (const char*)(torrentBackup.path().toUtf8()) << "'\n";
      exit(1);
    }
  }
  // Processing torrents
  file = path.trimmed().replace("file://", "");
  if(file.isEmpty()){
    return;
  }
  qDebug("Adding %s to download list", (const char*)file.toUtf8());
  std::ifstream in((const char*)file.toUtf8(), std::ios_base::binary);
  in.unsetf(std::ios_base::skipws);
  try{
    // Decode torrent file
    entry e = bdecode(std::istream_iterator<char>(in), std::istream_iterator<char>());
    // Getting torrent file informations
    torrent_info t(e);
    QString hash = QString(misc::toString(t.info_hash()).c_str());
    if(onStartup){
      qDebug("Added a hash to the unchecked torrents list");
      torrentsUnchecked << hash;
    }
    if(s->find_torrent(t.info_hash()).is_valid()){
      // Update info Bar
      if(!fromScanDir){
        if(!from_url.isNull()){
          emit duplicateTorrent(from_url);
        }else{
          emit duplicateTorrent(file);
        }
      }else{
        // Delete torrent from scan dir
        QFile::remove(file);
      }
      return;
    }
    // TODO: Remove this in a few releases (just for backward compatibility)
    if(torrentBackup.exists(QString(t.name().c_str())+".torrent")){
      QFile::rename(torrentBackup.path()+QDir::separator()+QString(t.name().c_str())+".torrent", torrentBackup.path()+QDir::separator()+hash+".torrent");
      QFile::rename(torrentBackup.path()+QDir::separator()+QString(t.name().c_str())+".fastresume", torrentBackup.path()+QDir::separator()+hash+".fastresume");
      QFile::rename(torrentBackup.path()+QDir::separator()+QString(t.name().c_str())+".savepath", torrentBackup.path()+QDir::separator()+hash+".savepath");
      QFile::rename(torrentBackup.path()+QDir::separator()+QString(t.name().c_str())+".paused", torrentBackup.path()+QDir::separator()+hash+".paused");
      QFile::rename(torrentBackup.path()+QDir::separator()+QString(t.name().c_str())+".incremental", torrentBackup.path()+QDir::separator()+hash+".incremental");
      file = torrentBackup.path() + QDir::separator() + hash + ".torrent";

    }
    //Getting fast resume data if existing
    if(torrentBackup.exists(hash+".fastresume")){
      try{
        std::stringstream strStream;
        strStream << hash.toStdString() << ".fastresume";
        boost::filesystem::ifstream resume_file(fs::path((const char*)torrentBackup.path().toUtf8()) / strStream.str(), std::ios_base::binary);
        resume_file.unsetf(std::ios_base::skipws);
        resume_data = bdecode(std::istream_iterator<char>(resume_file), std::istream_iterator<char>());
        fastResume=true;
      }catch (invalid_encoding&) {}
      catch (fs::filesystem_error&) {}
    }
    QString savePath = getSavePath(hash);
    // Adding files to bittorrent session
    if(hasFilteredFiles(hash)){
      h = s->add_torrent(t, fs::path((const char*)savePath.toUtf8()), resume_data, false);
      qDebug("Full allocation mode");
    }else{
      h = s->add_torrent(t, fs::path((const char*)savePath.toUtf8()), resume_data, true);
      qDebug("Compact allocation mode");
    }
    if(!h.is_valid()){
      // No need to keep on, it failed.
      qDebug("/!\\ Error: Invalid handle");
      return;
    }
    // Is this really useful and appropriate ?
    //h.set_max_connections(60);
    h.set_max_uploads(-1);
    qDebug("Torrent hash is " +  hash.toUtf8());
    // Load filtered files
    loadFilteredFiles(h);
    // Load trackers
    bool loaded_trackers = loadTrackerFile(hash);
    // Doing this to order trackers well
    if(!loaded_trackers){
      saveTrackerFile(hash);
      loadTrackerFile(hash);
    }
    torrent_status torrentStatus = h.status();
    QString newFile = torrentBackup.path() + QDir::separator() + hash + ".torrent";
    if(file != newFile){
      // Delete file from torrentBackup directory in case it exists because
      // QFile::copy() do not overwrite
      QFile::remove(newFile);
      // Copy it to torrentBackup directory
      QFile::copy(file, newFile);
    }
    // Pause torrent if it was paused last time
    if(QFile::exists(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+hash+".paused")){
      torrentsToPauseAfterChecking << hash;
      qDebug("Adding a torrent to the torrentsToPauseAfterChecking list");
    }
    // Incremental download
    if(QFile::exists(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+hash+".incremental")){
      qDebug("Incremental download enabled for %s", t.name().c_str());
      h.set_sequenced_download_threshold(15);
    }
    // If download from url
    if(!from_url.isNull()){
      // remove temporary file
      QFile::remove(file);
    }
    // Delete from scan dir to avoid trying to download it again
    if(fromScanDir){
      QFile::remove(file);
    }
    // Send torrent addition signal
    if(!from_url.isNull()){
      emit addedTorrent(from_url, h, fastResume);
    }else{
      emit addedTorrent(file, h, fastResume);
    }
  }catch (invalid_encoding& e){ // Raised by bdecode()
    std::cerr << "Could not decode file, reason: " << e.what() << '\n';
    // Display warning to tell user we can't decode the torrent file
    if(!from_url.isNull()){
      emit invalidTorrent(from_url);
    }else{
      emit invalidTorrent(file);
    }
    if(fromScanDir){
      // Remove .corrupt file in case it already exists
      QFile::remove(file+".corrupt");
      //Rename file extension so that it won't display error message more than once
      QFile::rename(file,file+".corrupt");
    }
  }
  catch (invalid_torrent_file&){ // Raised by torrent_info constructor
    // Display warning to tell user we can't decode the torrent file
    if(!from_url.isNull()){
      emit invalidTorrent(from_url);
    }else{
      emit invalidTorrent(file);
    }
    if(fromScanDir){
      // Remove .corrupt file in case it already exists
      QFile::remove(file+".corrupt");
      //Rename file extension so that it won't display error message more than once
      QFile::rename(file,file+".corrupt");
    }
  }
}

QStringList bittorrent::getTorrentsToPauseAfterChecking() const{
  return torrentsToPauseAfterChecking;
}

// Set the maximum number of opened connections
void bittorrent::setMaxConnections(int maxConnec){
  s->set_max_connections(maxConnec);
}

// Check in .priorities file if the user filtered files
// in this torrent.
bool bittorrent::hasFilteredFiles(const QString& fileHash) const{
  QFile pieces_file(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+fileHash+".priorities");
  // Read saved file
  if(!pieces_file.open(QIODevice::ReadOnly | QIODevice::Text)){
    return false;
  }
  QByteArray pieces_text = pieces_file.readAll();
  pieces_file.close();
  QList<QByteArray> pieces_priorities_list = pieces_text.split('\n');
  unsigned int listSize = pieces_priorities_list.size();
  for(unsigned int i=0; i<listSize-1; ++i){
    int priority = pieces_priorities_list.at(i).toInt();
    if( priority < 0 || priority > 7){
      priority = 1;
    }
    if(!priority){
      return true;
    }
  }
  return false;
}

// get the size of the torrent without the filtered files
size_type bittorrent::torrentEffectiveSize(QString hash) const{
  torrent_handle h = getTorrentHandle(hash);
  torrent_info t = h.get_torrent_info();
  unsigned int nbFiles = t.num_files();
  if(!h.is_valid()){
    qDebug("/!\\ Error: Invalid handle");
    return t.total_size();
  }
  QFile pieces_file(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+hash+".priorities");
  // Read saved file
  if(!pieces_file.open(QIODevice::ReadOnly | QIODevice::Text)){
    qDebug("* Error: Couldn't open priorities file");
    return t.total_size();
  }
  QByteArray pieces_priorities = pieces_file.readAll();
  pieces_file.close();
  QList<QByteArray> pieces_priorities_list = pieces_priorities.split('\n');
  if((unsigned int)pieces_priorities_list.size() != nbFiles+1){
    std::cerr << "* Error: Corrupted priorities file\n";
    return t.total_size();
  }
  size_type effective_size = 0;
  for(unsigned int i=0; i<nbFiles; ++i){
    int priority = pieces_priorities_list.at(i).toInt();
    if( priority < 0 || priority > 7){
      priority = 1;
    }
    if(priority)
      effective_size += t.file_at(i).size;
  }
  return effective_size;
}

// Return DHT state
bool bittorrent::isDHTEnabled() const{
  return DHTEnabled;
}

// Enable DHT
void bittorrent::enableDHT(){
  if(!DHTEnabled){
    boost::filesystem::ifstream dht_state_file((const char*)(misc::qBittorrentPath()+QString("dht_state")).toUtf8(), std::ios_base::binary);
    dht_state_file.unsetf(std::ios_base::skipws);
    entry dht_state;
    try{
      dht_state = bdecode(std::istream_iterator<char>(dht_state_file), std::istream_iterator<char>());
    }catch (std::exception&) {}
    s->start_dht(dht_state);
    s->add_dht_router(std::make_pair(std::string("router.bittorrent.com"), 6881));
    s->add_dht_router(std::make_pair(std::string("router.utorrent.com"), 6881));
    s->add_dht_router(std::make_pair(std::string("router.bitcomet.com"), 6881));
    DHTEnabled = true;
    qDebug("DHT enabled");
  }
}

// Disable DHT
void bittorrent::disableDHT(){
  if(DHTEnabled){
    DHTEnabled = false;
    s->stop_dht();
    qDebug("DHT disabled");
  }
}

// Read pieces priorities from .priorities file
// and ask torrent_handle to consider them
void bittorrent::loadFilteredFiles(torrent_handle &h){
  torrent_info torrentInfo = h.get_torrent_info();
  unsigned int nbFiles = torrentInfo.num_files();
  if(!h.is_valid()){
    qDebug("/!\\ Error: Invalid handle");
    return;
  }
  QString fileHash = QString(misc::toString(torrentInfo.info_hash()).c_str());
  QFile pieces_file(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+fileHash+".priorities");
  // Read saved file
  if(!pieces_file.open(QIODevice::ReadOnly | QIODevice::Text)){
    qDebug("* Error: Couldn't open priorities file");
    return;
  }
  QByteArray pieces_priorities = pieces_file.readAll();
  pieces_file.close();
  QList<QByteArray> pieces_priorities_list = pieces_priorities.split('\n');
  if((unsigned int)pieces_priorities_list.size() != nbFiles+1){
    std::cerr << "* Error: Corrupted priorities file\n";
    return;
  }
  std::vector<int> v;
  for(unsigned int i=0; i<nbFiles; ++i){
    int priority = pieces_priorities_list.at(i).toInt();
    if( priority < 0 || priority > 7){
      priority = 1;
    }
    qDebug("Setting piece piority to %d", priority);
    v.push_back(priority);
  }
  h.prioritize_files(v);
}

// Save fastresume data for all torrents
// and remove them from the session
void bittorrent::saveFastResumeData(){
  qDebug("Saving fast resume data");
  QString file;
  QDir torrentBackup(misc::qBittorrentPath() + "BT_backup");
  // Checking if torrentBackup Dir exists
  // create it if it is not
  if(! torrentBackup.exists()){
    torrentBackup.mkpath(torrentBackup.path());
  }
  // Write fast resume data
  std::vector<torrent_handle> handles = s->get_torrents();
  for(unsigned int i=0; i<handles.size(); ++i){
    torrent_handle &h = handles[i];
    if(!h.is_valid()){
      qDebug("/!\\ Error: Invalid handle");
      continue;
    }
    // Pause download (needed before fast resume writing)
    h.pause();
    // Extracting resume data
    if (h.has_metadata()){
      QString fileHash = QString(misc::toString(h.info_hash()).c_str());
      if(QFile::exists(torrentBackup.path()+QDir::separator()+fileHash+".torrent")){
        // Remove old .fastresume data in case it exists
        QFile::remove(torrentBackup.path()+QDir::separator()+fileHash + ".fastresume");
        // Write fast resume data
        entry resumeData = h.write_resume_data();
        file = fileHash + ".fastresume";
        boost::filesystem::ofstream out(fs::path((const char*)torrentBackup.path().toUtf8()) / (const char*)file.toUtf8(), std::ios_base::binary);
        out.unsetf(std::ios_base::skipws);
        bencode(std::ostream_iterator<char>(out), resumeData);
      }
      // Save trackers
      saveTrackerFile(fileHash);
    }
    // Remove torrent
    s->remove_torrent(h);
  }
  qDebug("Fast resume data saved");
}

bool bittorrent::isFilePreviewPossible(const QString& hash) const{
  // See if there are supported files in the torrent
  torrent_handle h = s->find_torrent(misc::fromString<sha1_hash>((hash.toStdString())));
  if(!h.is_valid()){
    qDebug("/!\\ Error: Invalid handle");
    return false;
  }
  torrent_info torrentInfo = h.get_torrent_info();
  for(int i=0; i<torrentInfo.num_files(); ++i){
    QString fileName = QString(torrentInfo.file_at(i).path.leaf().c_str());
    QString extension = fileName.split('.').last().toUpper();
    if(supported_preview_extensions.indexOf(extension) >= 0){
      return true;
    }
  }
  return false;
}

// Scan the first level of the directory for torrent files
// and add them to download list
void bittorrent::scanDirectory(){
  QString file;
  if(!scan_dir.isNull()){
    QStringList to_add;
    QDir dir(scan_dir);
    QStringList files = dir.entryList(QDir::Files, QDir::Unsorted);
    foreach(file, files){
      QString fullPath = dir.path()+QDir::separator()+file;
      if(fullPath.endsWith(".torrent")){
        QFile::rename(fullPath, fullPath+QString(".old"));
        to_add << fullPath+QString(".old");
      }
    }
    emit scanDirFoundTorrents(to_add);
  }
}

void bittorrent::setDefaultSavePath(const QString& savepath){
  defaultSavePath = savepath;
}

// Enable directory scanning
void bittorrent::enableDirectoryScanning(const QString& _scan_dir){
  if(!_scan_dir.isEmpty()){
    scan_dir = _scan_dir;
    timerScan = new QTimer(this);
    connect(timerScan, SIGNAL(timeout()), this, SLOT(scanDirectory()));
    timerScan->start(5000);
  }
}

// Disable directory scanning
void bittorrent::disableDirectoryScanning(){
  if(!scan_dir.isNull()){
    scan_dir = QString::null;
    if(timerScan->isActive()){
      timerScan->stop();
    }
    delete timerScan;
  }
}

// Set the ports range in which is chosen the port the bittorrent
// session will listen to
void bittorrent::setListeningPortsRange(std::pair<unsigned short, unsigned short> ports){
  s->listen_on(ports);
}

// Set download rate limit
// -1 to disable
void bittorrent::setDownloadRateLimit(int rate){
  s->set_download_rate_limit(rate);
}

// Set upload rate limit
// -1 to disable
void bittorrent::setUploadRateLimit(int rate){
  s->set_upload_rate_limit(rate);
}

// libtorrent allow to adjust ratio for each torrent
// This function will apply to same ratio to all torrents
void bittorrent::setGlobalRatio(float ratio){
  std::vector<torrent_handle> handles = s->get_torrents();
  for(unsigned int i=0; i<handles.size(); ++i){
    torrent_handle h = handles[i];
    if(!h.is_valid()){
      qDebug("/!\\ Error: Invalid handle");
      continue;
    }
    h.set_ratio(ratio);
  }
}

bool bittorrent::loadTrackerFile(const QString& hash){
  QDir torrentBackup(misc::qBittorrentPath() + "BT_backup");
  QFile tracker_file(torrentBackup.path()+QDir::separator()+ hash + ".trackers");
  if(!tracker_file.exists()) return false;
  tracker_file.open(QIODevice::ReadOnly | QIODevice::Text);
  QStringList lines = QString(tracker_file.readAll().data()).split("\n");
  std::vector<announce_entry> trackers;
  QString line;
  foreach(line, lines){
    QStringList parts = line.split("|");
    if(parts.size() != 2) continue;
    announce_entry t(parts[0].toStdString());
    t.tier = parts[1].toInt();
    trackers.push_back(t);
  }
  if(trackers.size() != 0){
    torrent_handle h = getTorrentHandle(hash);
    h.replace_trackers(trackers);
    return true;
  }else{
    return false;
  }
}

void bittorrent::saveTrackerFile(const QString& hash){
  QDir torrentBackup(misc::qBittorrentPath() + "BT_backup");
  QFile tracker_file(torrentBackup.path()+QDir::separator()+ hash + ".trackers");
  if(tracker_file.exists()){
    tracker_file.remove();
  }
  tracker_file.open(QIODevice::WriteOnly | QIODevice::Text);
  torrent_handle h = getTorrentHandle(hash);
  std::vector<announce_entry> trackers = h.trackers();
  for(unsigned int i=0; i<trackers.size(); ++i){
    tracker_file.write(QByteArray(trackers[i].url.c_str())+QByteArray("|")+QByteArray(misc::toString(i).c_str())+QByteArray("\n"));
  }
  tracker_file.close();
}

// Pause all torrents in session
bool bittorrent::pauseAllTorrents(){
  bool paused_torrents = false;
  std::vector<torrent_handle> handles = s->get_torrents();
  for(unsigned int i=0; i<handles.size(); ++i){
    torrent_handle h = handles[i];
    if(h.is_valid() && !h.is_paused()){
      h.pause();
      paused_torrents = true;
    }
  }
  return paused_torrents;
}

// Resume all torrents in session
bool bittorrent::resumeAllTorrents(){
  bool resumed_torrents = false;
  std::vector<torrent_handle> handles = s->get_torrents();
  for(unsigned int i=0; i<handles.size(); ++i){
    torrent_handle h = handles[i];
    if(h.is_valid() && h.is_paused()){
      h.resume();
      resumed_torrents = true;
    }
  }
  return resumed_torrents;
}

// Add uT PeX extension to bittorrent session
void bittorrent::enablePeerExchange(){
  qDebug("Enabling Peer eXchange");
  s->add_extension(&create_ut_pex_plugin);
}

// Set DHT port (>= 1000)
void bittorrent::setDHTPort(int dht_port){
  if(dht_port >= 1000){
    struct dht_settings DHTSettings;
    DHTSettings.service_port = dht_port;
    s->set_dht_settings(DHTSettings);
    qDebug("Set DHT Port to %d", dht_port);
  }
}

// Enable IP Filtering
void bittorrent::enableIPFilter(ip_filter filter){
  qDebug("Enabling IPFiler");
  s->set_ip_filter(filter);
}

// Disable IP Filtering
void bittorrent::disableIPFilter(){
  qDebug("Disable IPFilter");
  s->set_ip_filter(ip_filter());
  qDebug("IPFilter disabled");
}

// Set BT session settings (user_agent)
void bittorrent::setSessionSettings(session_settings sessionSettings){
  qDebug("Set session settings");
  s->set_settings(sessionSettings);
}

// Set Proxy
void bittorrent::setProxySettings(proxy_settings proxySettings, bool trackers, bool peers, bool web_seeds, bool dht){
  qDebug("Set Proxy settings");
  if(trackers)
    s->set_tracker_proxy(proxySettings);
  if(peers)
    s->set_peer_proxy(proxySettings);
  if(web_seeds)
    s->set_web_seed_proxy(proxySettings);
  if(DHTEnabled && dht){
    s->set_dht_proxy(proxySettings);
  }
}

// Read alerts sent by the bittorrent session
void bittorrent::readAlerts(){
  // look at session alerts and display some infos
  std::auto_ptr<alert> a = s->pop_alert();
  while (a.get()){
    if (torrent_finished_alert* p = dynamic_cast<torrent_finished_alert*>(a.get())){
      emit finishedTorrent(p->handle);
    }
    else if (file_error_alert* p = dynamic_cast<file_error_alert*>(a.get())){
      emit fullDiskError(p->handle);
    }
    else if (dynamic_cast<listen_failed_alert*>(a.get())){
      // Level: fatal
      emit portListeningFailure();
    }
    else if (tracker_alert* p = dynamic_cast<tracker_alert*>(a.get())){
      // Level: fatal
      QString fileHash = QString(misc::toString(p->handle.info_hash()).c_str());
      emit trackerError(fileHash, QTime::currentTime().toString("hh:mm:ss"), QString(a->msg().c_str()));
      // Authentication
      if(p->status_code == 401){
        emit trackerAuthenticationRequired(p->handle);
      }
    }
    else if (peer_blocked_alert* p = dynamic_cast<peer_blocked_alert*>(a.get())){
      emit peerBlocked(QString(p->ip.to_string().c_str()));
    }
    a = s->pop_alert();
  }
}

void bittorrent::reloadTorrent(const torrent_handle &h, bool compact_mode){
  qDebug("** Reloading a torrent");
  if(!h.is_valid()){
    qDebug("/!\\ Error: Invalid handle");
    return;
  }
  QDir torrentBackup(misc::qBittorrentPath() + "BT_backup");
  fs::path saveDir = h.save_path();
  QString fileName = QString(h.name().c_str());
  QString fileHash = QString(misc::toString(h.info_hash()).c_str());
  qDebug("Reloading torrent: %s", (const char*)fileName.toUtf8());
  torrent_handle new_h;
  entry resumeData;
  torrent_info t = h.get_torrent_info();
    // Checking if torrentBackup Dir exists
  // create it if it is not
  if(! torrentBackup.exists()){
    torrentBackup.mkpath(torrentBackup.path());
  }
  // Write fast resume data
  // Pause download (needed before fast resume writing)
  h.pause();
  // Extracting resume data
  if (h.has_metadata()){
    // get fast resume data
    resumeData = h.write_resume_data();
  }
  // Remove torrent
  s->remove_torrent(h);
  // Add torrent again to session
  unsigned short timeout = 0;
  while(h.is_valid() && timeout < 6){
    SleeperThread::msleep(1000);
    ++timeout;
  }
  if(h.is_valid()){
    std::cerr << "Error: Couldn't reload the torrent\n";
    return;
  }
  new_h = s->add_torrent(t, saveDir, resumeData, compact_mode);
  if(compact_mode){
    qDebug("Using compact allocation mode");
  }else{
    qDebug("Using full allocation mode");
  }

//   new_h.set_max_connections(60);
  new_h.set_max_uploads(-1);
  // Load filtered Files
  loadFilteredFiles(new_h);

  // Pause torrent if it was paused last time
  if(QFile::exists(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+fileHash+".paused")){
    new_h.pause();
  }
  // Incremental download
  if(QFile::exists(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+fileHash+".incremental")){
    qDebug("Incremental download enabled for %s", (const char*)fileName.toUtf8());
    new_h.set_sequenced_download_threshold(15);
  }
  emit updateFileSize(fileHash);
}

int bittorrent::getListenPort() const{
  return s->listen_port();
}

session_status bittorrent::getSessionStatus() const{
  return s->status();
}

QString bittorrent::getSavePath(const QString& hash){
  QFile savepath_file(misc::qBittorrentPath()+"BT_backup"+QDir::separator()+hash+".savepath");
  QByteArray line;
  QString savePath;
  if(savepath_file.open(QIODevice::ReadOnly | QIODevice::Text)){
    line = savepath_file.readAll();
    savepath_file.close();
    qDebug("Save path: %s", line.data());
    savePath = QString::fromUtf8(line.data());
  }else{
    // use default save path
    savePath = defaultSavePath;
  }
  // Checking if savePath Dir exists
  // create it if it is not
  QDir saveDir(savePath);
  if(!saveDir.exists()){
    if(!saveDir.mkpath(saveDir.path())){
      std::cerr << "Couldn't create the save directory: " << (const char*)saveDir.path().toUtf8() << "\n";
      // XXX: handle this better
      return QDir::homePath();
    }
  }
  return savePath;
}

// Take an url string to a torrent file,
// download the torrent file to a tmp location, then
// add it to download list
void bittorrent::downloadFromUrl(const QString& url){
  emit aboutToDownloadFromUrl(url);
  // Launch downloader thread
  downloader->downloadUrl(url);
}

// Add to bittorrent session the downloaded torrent file
void bittorrent::processDownloadedFile(const QString& url, const QString& file_path, int return_code, const QString& errorBuffer){
  if(return_code){
    // Download failed
    emit downloadFromUrlFailure(url, errorBuffer);
    QFile::remove(file_path);
    return;
  }
  // Add file to torrent download list
  emit newDownloadedTorrent(file_path, url);
}

void bittorrent::downloadFromURLList(const QStringList& url_list){
  QString url;
  qDebug("DownloadFromUrlList");
  foreach(url, url_list){
    downloadFromUrl(url);
  }
}

// Return current download rate for the BT
// session. Payload means that it only take into
// account "useful" part of the rate
float bittorrent::getPayloadDownloadRate() const{
  session_status sessionStatus = s->status();
  return sessionStatus.payload_download_rate;
}

// Return current upload rate for the BT
// session. Payload means that it only take into
// account "useful" part of the rate
float bittorrent::getPayloadUploadRate() const{
  session_status sessionStatus = s->status();
  return sessionStatus.payload_upload_rate;
}

// Return a vector with all torrent handles in it
std::vector<torrent_handle> bittorrent::getTorrentHandles() const{
  return s->get_torrents();
}

// Return a vector with all finished torrent handles in it
QList<torrent_handle> bittorrent::getFinishedTorrentHandles() const{
  QList<torrent_handle> finished;
  std::vector<torrent_handle> handles;
  for(unsigned int i=0; i<handles.size(); ++i){
    torrent_handle h = handles[i];
    if(h.is_seed()){
      finished << h;
    }
  }
  return finished;
}

QStringList bittorrent::getUncheckedTorrentsList() const{
  return torrentsUnchecked;
}

void bittorrent::setTorrentFinishedChecking(QString hash){
  int index = torrentsUnchecked.indexOf(hash);
  qDebug("torrent %s finished checking", (const char*)hash.toUtf8());
  if(index != -1){
    torrentsUnchecked.removeAt(index);
    qDebug("Still %d unchecked torrents", torrentsUnchecked.size());
    if(torrentsUnchecked.size() == 0){
      emit allTorrentsFinishedChecking();
    }
  }
}

// Save DHT entry to hard drive
void bittorrent::saveDHTEntry(){
  // Save DHT entry
  if(DHTEnabled){
    try{
      entry dht_state = s->dht_state();
      boost::filesystem::ofstream out((const char*)(misc::qBittorrentPath()+QString("dht_state")).toUtf8(), std::ios_base::binary);
      out.unsetf(std::ios_base::skipws);
      bencode(std::ostream_iterator<char>(out), dht_state);
    }catch (std::exception& e){
      std::cerr << e.what() << "\n";
    }
  }
}

void bittorrent::applyEncryptionSettings(pe_settings se){
  qDebug("Applying encryption settings");
  s->set_pe_settings(se);
}

// Will fast resume unfinished torrents in
// backup directory
void bittorrent::resumeUnfinished(){
  qDebug("Resuming unfinished torrents");
  QDir torrentBackup(misc::qBittorrentPath() + "BT_backup");
  QStringList fileNames, filePaths;
  // Scan torrentBackup directory
  fileNames = torrentBackup.entryList();
  QString fileName;
  foreach(fileName, fileNames){
    if(fileName.endsWith(".torrent")){
      filePaths.append(torrentBackup.path()+QDir::separator()+fileName);
    }
  }
  // Resume downloads
  foreach(fileName, filePaths){
    addTorrent(fileName, false, true);
  }
  qDebug("Unfinished torrents resumed");
}
