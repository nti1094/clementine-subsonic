/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "playlist.h"
#include "playlistbackend.h"
#include "playlistcontainer.h"
#include "playlistmanager.h"
#include "playlistview.h"
#include "specialplaylisttype.h"
#include "core/application.h"
#include "core/logging.h"
#include "core/player.h"
#include "core/songloader.h"
#include "core/utilities.h"
#include "library/librarybackend.h"
#include "library/libraryplaylistitem.h"
#include "playlistparsers/playlistparser.h"
#include "smartplaylists/generator.h"

#include <QFileInfo>
#include <QtDebug>

using smart_playlists::GeneratorPtr;

PlaylistManager::PlaylistManager(Application* app, QObject *parent)
  : PlaylistManagerInterface(app, parent),
    app_(app),
    playlist_backend_(NULL),
    library_backend_(NULL),
    sequence_(NULL),
    parser_(NULL),
    default_playlist_type_(new DefaultPlaylistType),
    current_(-1),
    active_(-1)
{
  connect(app_->player(), SIGNAL(Paused()), SLOT(SetActivePaused()));
  connect(app_->player(), SIGNAL(Playing()), SLOT(SetActivePlaying()));
  connect(app_->player(), SIGNAL(Stopped()), SLOT(SetActiveStopped()));
}

PlaylistManager::~PlaylistManager() {
  foreach (const Data& data, playlists_.values()) {
    delete data.p;
  }

  qDeleteAll(special_playlist_types_.values());
  delete default_playlist_type_;
}

void PlaylistManager::Init(LibraryBackend* library_backend,
                           PlaylistBackend* playlist_backend,
                           PlaylistSequence* sequence,
                           PlaylistContainer* playlist_container) {
  library_backend_ = library_backend;
  playlist_backend_ = playlist_backend;
  sequence_ = sequence;
  parser_ = new PlaylistParser(library_backend, this);
  playlist_container_ = playlist_container;

  connect(library_backend_, SIGNAL(SongsDiscovered(SongList)), SLOT(SongsDiscovered(SongList)));
  connect(library_backend_, SIGNAL(SongsStatisticsChanged(SongList)), SLOT(SongsDiscovered(SongList)));

  foreach (const PlaylistBackend::Playlist& p, playlist_backend->GetAllPlaylists()) {
    AddPlaylist(p.id, p.name, p.special_type);
  }

  // If no playlist exists then make a new one
  if (playlists_.isEmpty())
    New(tr("Playlist"));

  emit PlaylistManagerInitialized();
}

QList<Playlist*> PlaylistManager::GetAllPlaylists() const {
  QList<Playlist*> result;

  foreach(const Data& data, playlists_.values()) {
    result.append(data.p);
  }

  return result;
}

QItemSelection PlaylistManager::selection(int id) const {
  QMap<int, Data>::const_iterator it = playlists_.find(id);
  return it->selection;
}

Playlist* PlaylistManager::AddPlaylist(int id, const QString& name,
                                       const QString& special_type) {
  Playlist* ret = new Playlist(playlist_backend_, app_->task_manager(),
                               library_backend_, id, special_type);
  ret->set_sequence(sequence_);

  connect(ret, SIGNAL(CurrentSongChanged(Song)), SIGNAL(CurrentSongChanged(Song)));
  connect(ret, SIGNAL(PlaylistChanged()), SLOT(OneOfPlaylistsChanged()));
  connect(ret, SIGNAL(PlaylistChanged()), SLOT(UpdateSummaryText()));
  connect(ret, SIGNAL(EditingFinished(QModelIndex)), SIGNAL(EditingFinished(QModelIndex)));
  connect(ret, SIGNAL(LoadTracksError(QString)), SIGNAL(Error(QString)));
  connect(ret, SIGNAL(PlayRequested(QModelIndex)), SIGNAL(PlayRequested(QModelIndex)));
  connect(playlist_container_->view(), SIGNAL(ColumnAlignmentChanged(ColumnAlignmentMap)),
          ret, SLOT(SetColumnAlignment(ColumnAlignmentMap)));

  playlists_[id] = Data(ret, name);

  emit PlaylistAdded(id, name);

  if (current_ == -1) {
    SetCurrentPlaylist(id);
  }
  if (active_ == -1) {
    SetActivePlaylist(id);
  }

  return ret;
}

void PlaylistManager::New(const QString& name, const SongList& songs,
                          const QString& special_type) {
  if (name.isNull())
    return;

  int id = playlist_backend_->CreatePlaylist(name, special_type);

  if (id == -1)
    qFatal("Couldn't create playlist");

  Playlist* playlist = AddPlaylist(id, name, special_type);
  playlist->InsertSongsOrLibraryItems(songs);

  SetCurrentPlaylist(id);
}

void PlaylistManager::Load(const QString& filename) {
  QUrl url = QUrl::fromLocalFile(filename);
  SongLoader* loader = new SongLoader(library_backend_, this);
  connect(loader, SIGNAL(LoadFinished(bool)), SLOT(LoadFinished(bool)));
  SongLoader::Result result = loader->Load(url);
  QFileInfo info(filename);

  if (result == SongLoader::Error ||
      (result == SongLoader::Success && loader->songs().isEmpty())) {
    app_->AddError(tr("The playlist '%1' was empty or could not be loaded.").arg(
        info.completeBaseName()));
    delete loader;
    return;
  }

  if (result == SongLoader::Success) {
    New(info.baseName(), loader->songs());
    delete loader;
  }
}

void PlaylistManager::LoadFinished(bool success) {
  SongLoader* loader = qobject_cast<SongLoader*>(sender());
  loader->deleteLater();
  QString localfile = loader->url().toLocalFile();
  QFileInfo info(localfile);
  if (!success || loader->songs().isEmpty()) {
    app_->AddError(tr("The playlist '%1' was empty or could not be loaded.").arg(
        info.completeBaseName()));
  }

  New(info.baseName(), loader->songs());
}

void PlaylistManager::Save(int id, const QString& filename) {
  Q_ASSERT(playlists_.contains(id));

  parser_->Save(playlist(id)->GetAllSongs(), filename);
}

void PlaylistManager::Rename(int id, const QString& new_name) {
  Q_ASSERT(playlists_.contains(id));

  playlist_backend_->RenamePlaylist(id, new_name);
  playlists_[id].name = new_name;

  emit PlaylistRenamed(id, new_name);
}

void PlaylistManager::Remove(int id) {
  Q_ASSERT(playlists_.contains(id));

  // Won't allow removing the last playlist
  if (playlists_.count() <= 1)
    return;

  playlist_backend_->RemovePlaylist(id);

  int next_id = -1;
  foreach (int possible_next_id, playlists_.keys()) {
    if (possible_next_id != id) {
      next_id = possible_next_id;
      break;
    }
  }
  if (next_id == -1)
    return;

  if (id == active_)
    SetActivePlaylist(next_id);
  if (id == current_)
    SetCurrentPlaylist(next_id);

  Data data = playlists_.take(id);
  delete data.p;

  emit PlaylistRemoved(id);
}

void PlaylistManager::OneOfPlaylistsChanged() {
  emit PlaylistChanged(qobject_cast<Playlist*>(sender()));
}

void PlaylistManager::SetCurrentPlaylist(int id) {
  Q_ASSERT(playlists_.contains(id));
  current_ = id;
  emit CurrentChanged(current());
  UpdateSummaryText();
}

void PlaylistManager::SetActivePlaylist(int id) {
  Q_ASSERT(playlists_.contains(id));

  // Kinda a hack: unset the current item from the old active playlist before
  // setting the new one
  if (active_ != -1 && active_ != id)
    active()->set_current_row(-1);

  active_ = id;
  emit ActiveChanged(active());

  sequence_->SetUsingDynamicPlaylist(active()->is_dynamic());
}

void PlaylistManager::ClearCurrent() {
  current()->Clear();
}

void PlaylistManager::ShuffleCurrent() {
  current()->Shuffle();
}

void PlaylistManager::RemoveDuplicatesCurrent() {
  current()->RemoveDuplicateSongs();
}

void PlaylistManager::SetActivePlaying() {
  active()->Playing();
}

void PlaylistManager::SetActivePaused() {
  active()->Paused();
}

void PlaylistManager::SetActiveStopped() {
  active()->Stopped();
}

void PlaylistManager::SetActiveStreamMetadata(const QUrl &url, const Song &song) {
  active()->SetStreamMetadata(url, song);
}

void PlaylistManager::RateCurrentSong(double rating) {
  active()->RateSong(active()->current_index(), rating);
}

void PlaylistManager::RateCurrentSong(int rating) {
  RateCurrentSong(rating / 5.0);
}

void PlaylistManager::ChangePlaylistOrder(const QList<int>& ids) {
  playlist_backend_->SetPlaylistOrder(ids);
}

void PlaylistManager::UpdateSummaryText() {
  int tracks = current()->rowCount();
  quint64 nanoseconds = 0;
  int selected = 0;

  // Get the length of the selected tracks
  foreach (const QItemSelectionRange& range, playlists_[current_id()].selection) {
    if (!range.isValid())
      continue;

    selected += range.bottom() - range.top() + 1;
    for (int i=range.top() ; i<=range.bottom() ; ++i) {
      qint64 length = range.model()->index(i, Playlist::Column_Length).data().toLongLong();
      if (length > 0)
        nanoseconds += length;
    }
  }

  QString summary;
  if (selected > 1) {
    summary += tr("%1 selected of").arg(selected) + " ";
  } else {
    nanoseconds = current()->GetTotalLength();
  }

  // TODO: Make the plurals translatable
  summary += tracks == 1 ? tr("1 track") : tr("%1 tracks").arg(tracks);

  if (nanoseconds)
    summary += " - [ " + Utilities::WordyTimeNanosec(nanoseconds) + " ]";

  emit SummaryTextChanged(summary);
}

void PlaylistManager::SelectionChanged(const QItemSelection& selection) {
  playlists_[current_id()].selection = selection;
  UpdateSummaryText();
}

void PlaylistManager::SongsDiscovered(const SongList& songs) {
  // Some songs might've changed in the library, let's update any playlist
  // items we have that match those songs

  foreach (const Song& song, songs) {
    foreach (const Data& data, playlists_) {
      PlaylistItemList items = data.p->library_items_by_id(song.id());
      foreach (PlaylistItemPtr item, items) {
        if (item->Metadata().directory_id() != song.directory_id())
          continue;
        static_cast<LibraryPlaylistItem*>(item.get())->SetMetadata(song);
        data.p->ItemChanged(item);
      }
    }
  }
}

void PlaylistManager::PlaySmartPlaylist(GeneratorPtr generator, bool as_new, bool clear) {
  if (as_new) {
    New(generator->name());
  }

  if (clear) {
    current()->Clear();
  }

  current()->InsertSmartPlaylist(generator);
}

// When Player has processed the new song chosen by the user...
void PlaylistManager::SongChangeRequestProcessed(const QUrl& url, bool valid) {
  foreach(Playlist* playlist, GetAllPlaylists()) {
    if(playlist->ApplyValidityOnCurrentSong(url, valid)) {
      return;
    }
  }
}

void PlaylistManager::InvalidateDeletedSongs() {
  foreach(Playlist* playlist, GetAllPlaylists()) {
    playlist->InvalidateDeletedSongs();
  }
}

void PlaylistManager::RemoveDeletedSongs() {
  foreach(Playlist* playlist, GetAllPlaylists()) {
    playlist->RemoveDeletedSongs();
  }
}

QString PlaylistManager::GetNameForNewPlaylist(const SongList& songs) {
  if (songs.isEmpty()) {
    return tr("Playlist");
  }

  QSet<QString> artists;
  QSet<QString> albums;

  foreach(const Song& song, songs) {
    artists << (song.artist().isEmpty()
                    ? tr("Unknown")
                    : song.artist());
    albums << (song.album().isEmpty()
                    ? tr("Unknown")
                    : song.album());

    if(artists.size() > 1) {
      break;
    }
  }

  bool various_artists = artists.size() > 1;

  QString result;
  if(various_artists) {
    result = tr("Various artists");
  } else {
    result = artists.values().first();
  }

  if(!various_artists && albums.size() == 1) {
    result += " - " + albums.toList().first();
  }

  return result;
}

void PlaylistManager::RegisterSpecialPlaylistType(SpecialPlaylistType* type) {
  const QString name = type->name();

  if (special_playlist_types_.contains(name)) {
    qLog(Warning) << "Tried to register a special playlist type" << name
                  << "but one was already registered";
    return;
  }

  qLog(Info) << "Registered special playlist type" << name;
  special_playlist_types_.insert(name, type);
}

void PlaylistManager::UnregisterSpecialPlaylistType(SpecialPlaylistType* type) {
  const QString name = special_playlist_types_.key(type);
  if (name.isEmpty()) {
    qLog(Warning) << "Tried to unregister a special playlist type" << type->name()
                  << "that wasn't registered";
    return;
  }

  qLog(Info) << "Unregistered special playlist type" << name;
  special_playlist_types_.remove(name);
}

SpecialPlaylistType* PlaylistManager::GetPlaylistType(const QString& type) const {
  if (special_playlist_types_.contains(type)) {
    return special_playlist_types_[type];
  }
  return default_playlist_type_;
}
