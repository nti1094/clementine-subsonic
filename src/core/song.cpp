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

#include "mpris_common.h"
#include "song.h"
#include "timeconstants.h"
#include "core/encoding.h"
#include "core/logging.h"
#include "core/messagehandler.h"

#include <algorithm>

#ifdef HAVE_LIBLASTFM
  #include "internet/fixlastfm.h"
  #ifdef HAVE_LIBLASTFM1
    #include <lastfm/Track.h>
  #else
    #include <lastfm/Track>
  #endif
#endif

#include <QFile>
#include <QFileInfo>
#include <QLatin1Literal>
#include <QSharedData>
#include <QSqlQuery>
#include <QTextCodec>
#include <QTime>
#include <QVariant>
#include <QtConcurrentRun>

#include <id3v1genres.h>

#ifdef Q_OS_WIN32
# ifdef HAVE_SAC
#  include <mswmdm.h>
# endif
# include <QUuid>
#endif // Q_OS_WIN32

#ifdef HAVE_LIBGPOD
# include <gpod/itdb.h>
#endif

#ifdef HAVE_LIBMTP
# include <libmtp.h>
#endif

#include <boost/scoped_array.hpp>
#include <boost/scoped_ptr.hpp>
using boost::scoped_ptr;

#include "utilities.h"
#include "covers/albumcoverloader.h"
#include "engines/enginebase.h"
#include "library/sqlrow.h"
#include "widgets/trackslider.h"


const QStringList Song::kColumns = QStringList()
    << "title" << "album" << "artist" << "albumartist" << "composer" << "track"
    << "disc" << "bpm" << "year" << "genre" << "comment" << "compilation"
    << "bitrate" << "samplerate" << "directory" << "filename"
    << "mtime" << "ctime" << "filesize" << "sampler" << "art_automatic"
    << "art_manual" << "filetype" << "playcount" << "lastplayed" << "rating"
    << "forced_compilation_on" << "forced_compilation_off"
    << "effective_compilation" << "skipcount" << "score" << "beginning" << "length"
    << "cue_path" << "unavailable" << "effective_albumartist";

const QString Song::kColumnSpec = Song::kColumns.join(", ");
const QString Song::kBindSpec = Utilities::Prepend(":", Song::kColumns).join(", ");
const QString Song::kUpdateSpec = Utilities::Updateify(Song::kColumns).join(", ");


const QStringList Song::kFtsColumns = QStringList()
    << "ftstitle" << "ftsalbum" << "ftsartist" << "ftsalbumartist"
    << "ftscomposer" << "ftsgenre" << "ftscomment";

const QString Song::kFtsColumnSpec = Song::kFtsColumns.join(", ");
const QString Song::kFtsBindSpec = Utilities::Prepend(":", Song::kFtsColumns).join(", ");
const QString Song::kFtsUpdateSpec = Utilities::Updateify(Song::kFtsColumns).join(", ");

const QString Song::kManuallyUnsetCover = "(unset)";
const QString Song::kEmbeddedCover = "(embedded)";


struct Song::Private : public QSharedData {
  Private();

  bool valid_;
  int id_;

  QString title_;
  QString album_;
  QString artist_;
  QString albumartist_;
  QString composer_;
  int track_;
  int disc_;
  float bpm_;
  int year_;
  QString genre_;
  QString comment_;
  bool compilation_;            // From the file tag
  bool sampler_;                // From the library scanner
  bool forced_compilation_on_;  // Set by the user
  bool forced_compilation_off_; // Set by the user

  float rating_;
  int playcount_;
  int skipcount_;
  int lastplayed_;
  int score_;

  // The beginning of the song in seconds. In case of single-part media
  // streams, this will equal to 0. In case of multi-part streams on the
  // other hand, this will mark the beginning of a section represented by
  // this Song object. This is always greater than 0.
  qint64 beginning_;
  // The end of the song in seconds. In case of single-part media
  // streams, this will equal to the song's length. In case of multi-part
  // streams on the other hand, this will mark the end of a section
  // represented by this Song object.
  // This may be negative indicating that the length of this song is
  // unknown.
  qint64 end_;

  int bitrate_;
  int samplerate_;

  int directory_id_;
  QUrl url_;
  QString basefilename_;
  int mtime_;
  int ctime_;
  int filesize_;
  FileType filetype_;

  // If the song has a CUE, this contains it's path.
  QString cue_path_;

  // Filenames to album art for this song.
  QString art_automatic_; // Guessed by LibraryWatcher
  QString art_manual_;    // Set by the user - should take priority

  QImage image_;

  // Whether this song was loaded from a file using taglib.
  bool init_from_file_;
  // Whether our encoding guesser thinks these tags might be incorrectly encoded.
  bool suspicious_tags_;

  // Whether the song does not exist on the file system anymore, but is still
  // stored in the database so as to remember the user's metadata.
  bool unavailable_;
};


Song::Private::Private()
  : valid_(false),
    id_(-1),
    track_(-1),
    disc_(-1),
    bpm_(-1),
    year_(-1),
    compilation_(false),
    sampler_(false),
    forced_compilation_on_(false),
    forced_compilation_off_(false),
    rating_(-1.0),
    playcount_(0),
    skipcount_(0),
    lastplayed_(-1),
    score_(0),
    beginning_(0),
    end_(-1),
    bitrate_(-1),
    samplerate_(-1),
    directory_id_(-1),
    mtime_(-1),
    ctime_(-1),
    filesize_(-1),
    filetype_(Type_Unknown),
    init_from_file_(false),
    suspicious_tags_(false),
    unavailable_(false)
{
}


Song::Song()
  : d(new Private)
{
}

Song::Song(const Song &other)
  : d(other.d)
{
}

Song::~Song() {
}

Song& Song::operator =(const Song& other) {
  d = other.d;
  return *this;
}

bool Song::is_valid() const { return d->valid_; }
bool Song::is_unavailable() const { return d->unavailable_; }
int Song::id() const { return d->id_; }
const QString& Song::title() const { return d->title_; }
const QString& Song::album() const { return d->album_; }
const QString& Song::artist() const { return d->artist_; }
const QString& Song::albumartist() const { return d->albumartist_; }
const QString& Song::effective_albumartist() const { return d->albumartist_.isEmpty() ? d->artist_ : d->albumartist_; }
const QString& Song::playlist_albumartist() const { return is_compilation() ? d->albumartist_ : effective_albumartist(); }
const QString& Song::composer() const { return d->composer_; }
int Song::track() const { return d->track_; }
int Song::disc() const { return d->disc_; }
float Song::bpm() const { return d->bpm_; }
int Song::year() const { return d->year_; }
const QString& Song::genre() const { return d->genre_; }
const QString& Song::comment() const { return d->comment_; }
bool Song::is_compilation() const {
  return (d->compilation_ || d->sampler_ || d->forced_compilation_on_)
          && ! d->forced_compilation_off_;
}
float Song::rating() const { return d->rating_; }
int Song::playcount() const { return d->playcount_; }
int Song::skipcount() const { return d->skipcount_; }
int Song::lastplayed() const { return d->lastplayed_; }
int Song::score() const { return d->score_; }
const QString& Song::cue_path() const { return d->cue_path_; }
bool Song::has_cue() const { return !d->cue_path_.isEmpty(); }
qint64 Song::beginning_nanosec() const { return d->beginning_; }
qint64 Song::end_nanosec() const { return d->end_; }
qint64 Song::length_nanosec() const { return d->end_ - d->beginning_; }
int Song::bitrate() const { return d->bitrate_; }
int Song::samplerate() const { return d->samplerate_; }
int Song::directory_id() const { return d->directory_id_; }
const QUrl& Song::url() const { return d->url_; }
const QString& Song::basefilename() const { return d->basefilename_; }
uint Song::mtime() const { return d->mtime_; }
uint Song::ctime() const { return d->ctime_; }
int Song::filesize() const { return d->filesize_; }
Song::FileType Song::filetype() const { return d->filetype_; }
bool Song::is_stream() const { return d->filetype_ == Type_Stream; }
bool Song::is_cdda() const { return d->filetype_ == Type_Cdda; }
const QString& Song::art_automatic() const { return d->art_automatic_; }
const QString& Song::art_manual() const { return d->art_manual_; }
bool Song::has_manually_unset_cover() const { return d->art_manual_ == kManuallyUnsetCover; }
void Song::manually_unset_cover() { d->art_manual_ = kManuallyUnsetCover; }
bool Song::has_embedded_cover() const { return d->art_automatic_ == kEmbeddedCover; }
void Song::set_embedded_cover() { d->art_automatic_ = kEmbeddedCover; }
const QImage& Song::image() const { return d->image_; }
void Song::set_id(int id) { d->id_ = id; }
void Song::set_valid(bool v) { d->valid_ = v; }
void Song::set_title(const QString& v) { d->title_ = v; }
void Song::set_album(const QString& v) { d->album_ = v; }
void Song::set_artist(const QString& v) { d->artist_ = v; }
void Song::set_albumartist(const QString& v) { d->albumartist_ = v; }
void Song::set_composer(const QString& v) { d->composer_ = v; }
void Song::set_track(int v) { d->track_ = v; }
void Song::set_disc(int v) { d->disc_ = v; }
void Song::set_bpm(float v) { d->bpm_ = v; }
void Song::set_year(int v) { d->year_ = v; }
void Song::set_genre(const QString& v) { d->genre_ = v; }
void Song::set_comment(const QString& v) { d->comment_ = v; }
void Song::set_compilation(bool v) { d->compilation_ = v; }
void Song::set_sampler(bool v) { d->sampler_ = v; }
void Song::set_beginning_nanosec(qint64 v) { d->beginning_ = qMax(0ll, v); }
void Song::set_end_nanosec(qint64 v) { d->end_ = v; }
void Song::set_length_nanosec(qint64 v) { d->end_ = d->beginning_ + v; }
void Song::set_bitrate(int v) { d->bitrate_ = v; }
void Song::set_samplerate(int v) { d->samplerate_ = v; }
void Song::set_mtime(int v) { d->mtime_ = v; }
void Song::set_ctime(int v) { d->ctime_ = v; }
void Song::set_filesize(int v) { d->filesize_ = v; }
void Song::set_filetype(FileType v) { d->filetype_ = v; }
void Song::set_art_automatic(const QString& v) { d->art_automatic_ = v; }
void Song::set_art_manual(const QString& v) { d->art_manual_ = v; }
void Song::set_image(const QImage& i) { d->image_ = i; }
void Song::set_forced_compilation_on(bool v) { d->forced_compilation_on_ = v; }
void Song::set_forced_compilation_off(bool v) { d->forced_compilation_off_ = v; }
void Song::set_rating(float v) { d->rating_ = v; }
void Song::set_playcount(int v) { d->playcount_ = v; }
void Song::set_skipcount(int v) { d->skipcount_ = v; }
void Song::set_lastplayed(int v) { d->lastplayed_ = v; }
void Song::set_score(int v) { d->score_ = qBound(0, v, 100); }
void Song::set_cue_path(const QString& v) { d->cue_path_ = v; }
void Song::set_unavailable(bool v) { d->unavailable_ = v; }
void Song::set_url(const QUrl& v) { d->url_ = v; }
void Song::set_basefilename(const QString& v) { d->basefilename_ = v; }
void Song::set_directory_id(int v) { d->directory_id_ = v; }

QString Song::JoinSpec(const QString& table) {
  return Utilities::Prepend(table + ".", kColumns).join(", ");
}

QString Song::TextForFiletype(FileType type) {
  switch (type) {
    case Song::Type_Asf:       return QObject::tr("Windows Media audio");
    case Song::Type_Flac:      return QObject::tr("Flac");
    case Song::Type_Mp4:       return QObject::tr("MP4 AAC");
    case Song::Type_Mpc:       return QObject::tr("MPC");
    case Song::Type_Mpeg:      return QObject::tr("MP3"); // Not technically correct
    case Song::Type_OggFlac:   return QObject::tr("Ogg Flac");
    case Song::Type_OggSpeex:  return QObject::tr("Ogg Speex");
    case Song::Type_OggVorbis: return QObject::tr("Ogg Vorbis");
    case Song::Type_Aiff:      return QObject::tr("AIFF");
    case Song::Type_Wav:       return QObject::tr("Wav");
    case Song::Type_TrueAudio: return QObject::tr("TrueAudio");
    case Song::Type_Cdda:      return QObject::tr("CDDA");

    case Song::Type_Stream:    return QObject::tr("Stream");

    case Song::Type_Unknown:
    default:
      return QObject::tr("Unknown");
  }
}

int CompareSongsName(const Song& song1, const Song& song2) {
  return song1.PrettyTitleWithArtist().localeAwareCompare(song2.PrettyTitleWithArtist()) < 0;
}

void Song::SortSongsListAlphabetically(SongList* songs) {
  Q_ASSERT(songs);
  qSort(songs->begin(), songs->end(), CompareSongsName);
}

void Song::Init(const QString& title, const QString& artist,
                const QString& album, qint64 length_nanosec) {
  d->valid_ = true;

  d->title_ = title;
  d->artist_ = artist;
  d->album_ = album;

  set_length_nanosec(length_nanosec);
}

void Song::Init(const QString& title, const QString& artist,
                const QString& album, qint64 beginning, qint64 end) {
  d->valid_ = true;

  d->title_ = title;
  d->artist_ = artist;
  d->album_ = album;

  d->beginning_ = beginning;
  d->end_ = end;
}

void Song::set_genre_id3(int id) {
  set_genre(TStringToQString(TagLib::ID3v1::genre(id)));
}

QString Song::Decode(const QString& tag, const QTextCodec* codec) {
  if (!codec) {
    return tag;
  }

  return codec->toUnicode(tag.toUtf8());
}

void Song::InitFromProtobuf(const pb::tagreader::SongMetadata& pb) {
  d->init_from_file_ = true;
  d->valid_ = pb.valid();
  d->title_ = QStringFromStdString(pb.title());
  d->album_ = QStringFromStdString(pb.album());
  d->artist_ = QStringFromStdString(pb.artist());
  d->albumartist_ = QStringFromStdString(pb.albumartist());
  d->composer_ = QStringFromStdString(pb.composer());
  d->track_ = pb.track();
  d->disc_ = pb.disc();
  d->bpm_ = pb.bpm();
  d->year_ = pb.year();
  d->genre_ = QStringFromStdString(pb.genre());
  d->comment_ = QStringFromStdString(pb.comment());
  d->compilation_ = pb.compilation();
  d->playcount_ = pb.playcount();
  d->skipcount_ = pb.skipcount();
  d->lastplayed_ = pb.lastplayed();
  d->score_ = pb.score();
  set_length_nanosec(pb.length_nanosec());
  d->bitrate_ = pb.bitrate();
  d->samplerate_ = pb.samplerate();
  d->url_ = QUrl::fromEncoded(QByteArray(pb.url().data(), pb.url().size()));
  d->basefilename_ = QStringFromStdString(pb.basefilename());
  d->mtime_ = pb.mtime();
  d->ctime_ = pb.ctime();
  d->filesize_ = pb.filesize();
  d->suspicious_tags_ = pb.suspicious_tags();
  d->filetype_ = static_cast<FileType>(pb.type());

  if (pb.has_art_automatic()) {
    d->art_automatic_ = QStringFromStdString(pb.art_automatic());
  }

  if (pb.has_rating()) {
    d->rating_ = pb.rating();
  }
}

void Song::ToProtobuf(pb::tagreader::SongMetadata* pb) const {
  const QByteArray url(d->url_.toEncoded());

  pb->set_valid(d->valid_);
  pb->set_title(DataCommaSizeFromQString(d->title_));
  pb->set_album(DataCommaSizeFromQString(d->album_));
  pb->set_artist(DataCommaSizeFromQString(d->artist_));
  pb->set_albumartist(DataCommaSizeFromQString(d->albumartist_));
  pb->set_composer(DataCommaSizeFromQString(d->composer_));
  pb->set_track(d->track_);
  pb->set_disc(d->disc_);
  pb->set_bpm(d->bpm_);
  pb->set_year(d->year_);
  pb->set_genre(DataCommaSizeFromQString(d->genre_));
  pb->set_comment(DataCommaSizeFromQString(d->comment_));
  pb->set_compilation(d->compilation_);
  pb->set_rating(d->rating_);
  pb->set_playcount(d->playcount_);
  pb->set_skipcount(d->skipcount_);
  pb->set_lastplayed(d->lastplayed_);
  pb->set_score(d->score_);
  pb->set_length_nanosec(length_nanosec());
  pb->set_bitrate(d->bitrate_);
  pb->set_samplerate(d->samplerate_);
  pb->set_url(url.constData(), url.size());
  pb->set_basefilename(DataCommaSizeFromQString(d->basefilename_));
  pb->set_mtime(d->mtime_);
  pb->set_ctime(d->ctime_);
  pb->set_filesize(d->filesize_);
  pb->set_suspicious_tags(d->suspicious_tags_);
  pb->set_art_automatic(DataCommaSizeFromQString(d->art_automatic_));
  pb->set_type(static_cast< ::pb::tagreader::SongMetadata_Type>(d->filetype_));
}

void Song::InitFromQuery(const SqlRow& q, bool reliable_metadata, int col) {
  d->valid_ = true;
  d->init_from_file_ = reliable_metadata;

  #define tostr(n)      (q.value(n).isNull() ? QString::null : q.value(n).toString())
  #define tobytearray(n)(q.value(n).isNull() ? QByteArray()  : q.value(n).toByteArray())
  #define toint(n)      (q.value(n).isNull() ? -1 : q.value(n).toInt())
  #define tolonglong(n) (q.value(n).isNull() ? -1 : q.value(n).toLongLong())
  #define tofloat(n)    (q.value(n).isNull() ? -1 : q.value(n).toDouble())

  d->id_ = toint(col + 0);
  d->title_ = tostr(col + 1);
  d->album_ = tostr(col + 2);
  d->artist_ = tostr(col + 3);
  d->albumartist_ = tostr(col + 4);
  d->composer_ = tostr(col + 5);
  d->track_ = toint(col + 6);
  d->disc_ = toint(col + 7);
  d->bpm_ = tofloat(col + 8);
  d->year_ = toint(col + 9);
  d->genre_ = tostr(col + 10);
  d->comment_ = tostr(col + 11);
  d->compilation_ = q.value(col + 12).toBool();

  d->bitrate_ = toint(col + 13);
  d->samplerate_ = toint(col + 14);

  d->directory_id_ = toint(col + 15);
  d->url_ = QUrl::fromEncoded(tobytearray(col + 16));
  d->basefilename_ = QFileInfo(d->url_.toLocalFile()).fileName();
  d->mtime_ = toint(col + 17);
  d->ctime_ = toint(col + 18);
  d->filesize_ = toint(col + 19);

  d->sampler_ = q.value(col + 20).toBool();

  d->art_automatic_ = q.value(col + 21).toString();
  d->art_manual_ = q.value(col + 22).toString();

  d->filetype_ = FileType(q.value(col + 23).toInt());
  d->playcount_ = q.value(col + 24).isNull() ? 0 : q.value(col + 24).toInt();
  d->lastplayed_ = toint(col + 25);
  d->rating_ = tofloat(col + 26);

  d->forced_compilation_on_ = q.value(col + 27).toBool();
  d->forced_compilation_off_ = q.value(col + 28).toBool();

  // effective_compilation = 29

  d->skipcount_ = q.value(col + 30).isNull() ? 0 : q.value(col + 30).toInt();
  d->score_ = q.value(col + 31).isNull() ? 0 : q.value(col + 31).toInt();

  // do not move those statements - beginning must be initialized before
  // length is!
  d->beginning_ = q.value(col + 32).isNull() ? 0 : q.value(col + 32).toLongLong();
  set_length_nanosec(tolonglong(col + 33));

  d->cue_path_ = tostr(col + 34);
  d->unavailable_ = q.value(col + 35).toBool();

  // effective_albumartist = 36

  #undef tostr
  #undef toint
  #undef tolonglong
  #undef tofloat
}

void Song::InitFromFilePartial(const QString& filename) {
  d->url_ = QUrl::fromLocalFile(filename);
  // We currently rely on filename suffix to know if it's a music file or not.
  // TODO: I know this is not satisfying, but currently, we rely on TagLib
  // which seems to have the behavior (filename checks). Someday, it would be
  // nice to perform some magic tests everywhere.
  QFileInfo info(filename);
  d->basefilename_ = info.fileName();
  QString suffix = info.suffix().toLower();
  if (suffix == "mp3" || suffix == "ogg" || suffix == "flac" || suffix == "mpc"
      || suffix == "m4a" || suffix == "aac" || suffix == "wma" || suffix == "mp4"
      || suffix == "spx" || suffix == "wav") {
    d->valid_ = true;
  } else {
    d->valid_ = false;
  }
}

#ifdef HAVE_LIBLASTFM
void Song::InitFromLastFM(const lastfm::Track& track) {
  d->valid_ = true;
  d->filetype_ = Type_Stream;

  d->title_ = track.title();
  d->album_ = track.album();
  d->artist_ = track.artist();
  d->track_ = track.trackNumber();

  set_length_nanosec(track.duration() * kNsecPerSec);
}
#endif // HAVE_LIBLASTFM

#ifdef HAVE_LIBGPOD
  void Song::InitFromItdb(const Itdb_Track* track, const QString& prefix) {
    d->valid_ = true;

    d->title_ = QString::fromUtf8(track->title);
    d->album_ = QString::fromUtf8(track->album);
    d->artist_ = QString::fromUtf8(track->artist);
    d->albumartist_ = QString::fromUtf8(track->albumartist);
    d->composer_ = QString::fromUtf8(track->composer);
    d->track_ = track->track_nr;
    d->disc_ = track->cd_nr;
    d->bpm_ = track->BPM;
    d->year_ = track->year;
    d->genre_ = QString::fromUtf8(track->genre);
    d->comment_ = QString::fromUtf8(track->comment);
    d->compilation_ = track->compilation;
    set_length_nanosec(track->tracklen * kNsecPerMsec);
    d->bitrate_ = track->bitrate;
    d->samplerate_ = track->samplerate;
    d->mtime_ = track->time_modified;
    d->ctime_ = track->time_added;
    d->filesize_ = track->size;
    d->filetype_ = track->type2 ? Type_Mpeg : Type_Mp4;
    d->rating_ = float(track->rating) / 100; // 100 = 20 * 5 stars
    d->playcount_ = track->playcount;
    d->skipcount_ = track->skipcount;
    d->lastplayed_ = track->time_played;

    QString filename = QString::fromLocal8Bit(track->ipod_path);
    filename.replace(':', '/');

    if (prefix.contains("://")) {
      d->url_ = prefix + filename;
    } else {
      d->url_ = QUrl::fromLocalFile(prefix + filename);
    }

    d->basefilename_ = QFileInfo(filename).fileName();
  }

  void Song::ToItdb(Itdb_Track *track) const {
    track->title = strdup(d->title_.toUtf8().constData());
    track->album = strdup(d->album_.toUtf8().constData());
    track->artist = strdup(d->artist_.toUtf8().constData());
    track->albumartist = strdup(d->albumartist_.toUtf8().constData());
    track->composer = strdup(d->composer_.toUtf8().constData());
    track->track_nr = d->track_;
    track->cd_nr = d->disc_;
    track->BPM = d->bpm_;
    track->year = d->year_;
    track->genre = strdup(d->genre_.toUtf8().constData());
    track->comment = strdup(d->comment_.toUtf8().constData());
    track->compilation = d->compilation_;
    track->tracklen = length_nanosec() / kNsecPerMsec;
    track->bitrate = d->bitrate_;
    track->samplerate = d->samplerate_;
    track->time_modified = d->mtime_;
    track->time_added = d->ctime_;
    track->size = d->filesize_;
    track->type1 = 0;
    track->type2 = d->filetype_ == Type_Mp4 ? 0 : 1;
    track->mediatype = 1; // Audio
    track->rating = d->rating_ * 100; // 100 = 20 * 5 stars
    track->playcount = d->playcount_;
    track->skipcount = d->skipcount_;
    track->time_played = d->lastplayed_;
  }
#endif

#ifdef HAVE_LIBMTP
  void Song::InitFromMTP(const LIBMTP_track_t* track, const QString& host) {
    d->valid_ = true;

    d->title_ = QString::fromUtf8(track->title);
    d->artist_ = QString::fromUtf8(track->artist);
    d->album_ = QString::fromUtf8(track->album);
    d->composer_ = QString::fromUtf8(track->composer);
    d->genre_ = QString::fromUtf8(track->genre);
    d->url_ = QUrl(QString("mtp://%1/%2").arg(host, track->item_id));
    d->basefilename_ = QString::number(track->item_id);

    d->track_ = track->tracknumber;
    set_length_nanosec(track->duration * kNsecPerMsec);
    d->samplerate_ = track->samplerate;
    d->bitrate_ = track->bitrate;
    d->filesize_ = track->filesize;
    d->mtime_ = track->modificationdate;
    d->ctime_ = track->modificationdate;

    d->rating_ = float(track->rating) / 100;
    d->playcount_ = track->usecount;

    switch (track->filetype) {
      case LIBMTP_FILETYPE_WAV:  d->filetype_ = Type_Wav;       break;
      case LIBMTP_FILETYPE_MP3:  d->filetype_ = Type_Mpeg;      break;
      case LIBMTP_FILETYPE_WMA:  d->filetype_ = Type_Asf;       break;
      case LIBMTP_FILETYPE_OGG:  d->filetype_ = Type_OggVorbis; break;
      case LIBMTP_FILETYPE_MP4:  d->filetype_ = Type_Mp4;       break;
      case LIBMTP_FILETYPE_AAC:  d->filetype_ = Type_Mp4;       break;
      case LIBMTP_FILETYPE_FLAC: d->filetype_ = Type_OggFlac;   break;
      case LIBMTP_FILETYPE_MP2:  d->filetype_ = Type_Mpeg;      break;
      case LIBMTP_FILETYPE_M4A:  d->filetype_ = Type_Mp4;       break;
      default:                   d->filetype_ = Type_Unknown;   break;
    }
  }

  void Song::ToMTP(LIBMTP_track_t* track) const {
    track->item_id = 0;
    track->parent_id = 0;
    track->storage_id = 0;

    track->title = strdup(d->title_.toUtf8().constData());
    track->artist = strdup(d->artist_.toUtf8().constData());
    track->album = strdup(d->album_.toUtf8().constData());
    track->composer = strdup(d->composer_.toUtf8().constData());
    track->genre = strdup(d->genre_.toUtf8().constData());
    track->title = strdup(d->title_.toUtf8().constData());
    track->date = NULL;

    track->filename = strdup(d->basefilename_.toUtf8().constData());

    track->tracknumber = d->track_;
    track->duration = length_nanosec() / kNsecPerMsec;
    track->samplerate = d->samplerate_;
    track->nochannels = 0;
    track->wavecodec = 0;
    track->bitrate = d->bitrate_;
    track->bitratetype = 0;
    track->rating = d->rating_ * 100;
    track->usecount = d->playcount_;
    track->filesize = d->filesize_;
    track->modificationdate = d->mtime_;

    switch (d->filetype_) {
      case Type_Asf:       track->filetype = LIBMTP_FILETYPE_ASF;         break;
      case Type_Mp4:       track->filetype = LIBMTP_FILETYPE_MP4;         break;
      case Type_Mpeg:      track->filetype = LIBMTP_FILETYPE_MP3;         break;
      case Type_Flac:
      case Type_OggFlac:   track->filetype = LIBMTP_FILETYPE_FLAC;        break;
      case Type_OggSpeex:
      case Type_OggVorbis: track->filetype = LIBMTP_FILETYPE_OGG;         break;
      case Type_Wav:       track->filetype = LIBMTP_FILETYPE_WAV;         break;
      default:             track->filetype = LIBMTP_FILETYPE_UNDEF_AUDIO; break;
    }
  }
#endif

#if defined(Q_OS_WIN32) && defined(HAVE_SAC)
  static void AddWmdmItem(IWMDMMetaData* metadata, const wchar_t* name,
                          const QVariant& value) {
    switch (value.type()) {
      case QVariant::Int:
      case QVariant::UInt: {
        DWORD data = value.toUInt();
        metadata->AddItem(WMDM_TYPE_DWORD, name, (BYTE*)&data, sizeof(data));
        break;
      }
      case QVariant::String: {
        ScopedWCharArray data(value.toString());
        metadata->AddItem(WMDM_TYPE_STRING, name, (BYTE*)data.get(), data.bytes());
        break;
      }
      case QVariant::ByteArray: {
        QByteArray data = value.toByteArray();
        metadata->AddItem(WMDM_TYPE_BINARY, name, (BYTE*)data.constData(), data.size());
        break;
      }
      case QVariant::Bool: {
        int data = value.toBool();
        metadata->AddItem(WMDM_TYPE_BOOL, name, (BYTE*)&data, sizeof(data));
        break;
      }
      case QVariant::LongLong:
      case QVariant::ULongLong: {
        quint64 data = value.toULongLong();
        metadata->AddItem(WMDM_TYPE_QWORD, name, (BYTE*)&data, sizeof(data));
        break;
      }
      default:
        qLog(Warning) << "Type" << value.type() << "not handled";
        Q_ASSERT(0);
        break;
    }
  }

  static QVariant ReadWmdmValue(int type, uchar* data, uint length) {
    switch (type) {
    case WMDM_TYPE_DWORD:
      return QVariant::fromValue(uint(*reinterpret_cast<DWORD*>(data)));
    case WMDM_TYPE_WORD:
      return QVariant::fromValue(uint(*reinterpret_cast<WORD*>(data)));
    case WMDM_TYPE_QWORD:
      return QVariant::fromValue(qulonglong(*reinterpret_cast<quint64*>(data)));
    case WMDM_TYPE_STRING:
      return QString::fromWCharArray(reinterpret_cast<wchar_t*>(data), length/2);
    case WMDM_TYPE_BINARY:
      return QByteArray(reinterpret_cast<char*>(data), length);
    case WMDM_TYPE_BOOL:
      return bool(*reinterpret_cast<int*>(data));
    case WMDM_TYPE_GUID:
      return QUuid(*reinterpret_cast<GUID*>(data)).toString();
    }
    return QVariant();
  }

  void Song::InitFromWmdm(IWMDMMetaData* metadata) {
    bool non_consumable = false;
    int format = 0;

    // How much metadata is there?
    uint count = 0;
    metadata->GetItemCount(&count);

    for (int i=0 ; i<count ; ++i) {
      // Get this metadata item
      wchar_t* name = NULL;
      WMDM_TAG_DATATYPE type;
      BYTE* value = NULL;
      uint length = 0;

      metadata->QueryByIndex(i, &name, &type, &value, &length);

      QVariant item_value = ReadWmdmValue(type, value, length);

      // Store it in the song if it's something we recognise
      if (wcscmp(name, g_wszWMDMTitle) == 0)
        d->title_ = item_value.toString();

      else if (wcscmp(name, g_wszWMDMAuthor) == 0)
        d->artist_ = item_value.toString();

      else if (wcscmp(name, g_wszWMDMDescription) == 0)
        d->comment_ = item_value.toString();

      else if (wcscmp(name, g_wszWMDMAlbumTitle) == 0)
        d->album_ = item_value.toString();

      else if (wcscmp(name, g_wszWMDMTrack) == 0)
        d->track_ = item_value.toInt();

      else if (wcscmp(name, g_wszWMDMGenre) == 0)
        d->genre_ = item_value.toString();

      else if (wcscmp(name, g_wszWMDMYear) == 0)
        d->year_ = item_value.toInt();

      else if (wcscmp(name, g_wszWMDMComposer) == 0)
        d->composer_ = item_value.toString();

      else if (wcscmp(name, g_wszWMDMBitrate) == 0)
        d->bitrate_ = item_value.toInt();

      else if (wcscmp(name, g_wszWMDMFileName) == 0)
        d->url_ = QUrl::fromLocalFile(item_value.toString());

      else if (wcscmp(name, g_wszWMDMDuration) == 0)
        set_length_nanosec(item_value.toULongLong() * 1e2);

      else if (wcscmp(name, L"WMDM/FileSize") == 0)
        d->filesize_ = item_value.toULongLong();

      else if (wcscmp(name, L"WMDM/NonConsumable") == 0)
        non_consumable = item_value.toBool();

      else if (wcscmp(name, L"WMDM/FormatCode") == 0)
        format = item_value.toInt();

      CoTaskMemFree(name);
      CoTaskMemFree(value);
    }

    // Decide if this is music or not
    if (count == 0 || non_consumable)
      return;

    switch (format) {
    case WMDM_FORMATCODE_AIFF:
      d->filetype_ = Song::Type_Aiff;
      break;

    case WMDM_FORMATCODE_WAVE:
      d->filetype_ = Song::Type_Wav;
      break;

    case WMDM_FORMATCODE_MP2:
    case WMDM_FORMATCODE_MP3:
    case WMDM_FORMATCODE_MPEG:
      d->filetype_ = Song::Type_Mpeg;
      break;

    case WMDM_FORMATCODE_WMA:
    case WMDM_FORMATCODE_ASF:
      d->filetype_ = Song::Type_Asf;
      break;

    case WMDM_FORMATCODE_OGG:
      d->filetype_ = Song::Type_OggVorbis;
      break;

    case WMDM_FORMATCODE_AAC:
    case WMDM_FORMATCODE_MP4:
      d->filetype_ = Song::Type_Mp4;
      break;

    case WMDM_FORMATCODE_FLAC:
      d->filetype_ = Song::Type_Flac;
      break;

    case WMDM_FORMATCODE_AUDIBLE:
    case WMDM_FORMATCODE_UNDEFINEDAUDIO:
      d->filetype_ = Song::Type_Unknown;
      break;

    case WMDM_FORMATCODE_UNDEFINED:
      // WMDM doesn't know what type of file it is, so we start guessing - first
      // check if any of the music metadata fields were defined.  If they were,
      // there's a fairly good chance the file was music.
      if (!d->title_.isEmpty() || !d->artist_.isEmpty() ||
          !d->album_.isEmpty() || !d->comment_.isEmpty() ||
          !d->genre_.isEmpty() || d->track_ != -1 || d->year_ != -1 ||
          length_nanosec() != -1) {
        d->filetype_ = Song::Type_Unknown;
        break;
      }

      // Make a final guess based on the file extension
      {
        QString ext = d->url_.path().section('.', -1, -1).toLower();
        if (ext == "mp3" || ext == "wma" || ext == "flac" || ext == "ogg" ||
            ext == "spx" || ext == "mp4" || ext == "aac" || ext == "m4a")
          break;
      }
      return;

    default:
      return; // It's not music
    }

    d->valid_ = true;
    d->mtime_ = 0;
    d->ctime_ = 0;
  }

  void Song::ToWmdm(IWMDMMetaData* metadata) const {
    AddWmdmItem(metadata, g_wszWMDMTitle, d->title_);
    AddWmdmItem(metadata, g_wszWMDMAuthor, d->artist_);
    AddWmdmItem(metadata, g_wszWMDMDescription, d->comment_);
    AddWmdmItem(metadata, g_wszWMDMAlbumTitle, d->album_);
    AddWmdmItem(metadata, g_wszWMDMTrack, d->track_);
    AddWmdmItem(metadata, g_wszWMDMGenre, d->genre_);
    AddWmdmItem(metadata, g_wszWMDMYear, QString::number(d->year_));
    AddWmdmItem(metadata, g_wszWMDMComposer, d->composer_);
    AddWmdmItem(metadata, g_wszWMDMBitrate, d->bitrate_);
    AddWmdmItem(metadata, g_wszWMDMFileName, d->basefilename_);
    AddWmdmItem(metadata, g_wszWMDMDuration, qint64(length_nanosec() / 1e2));
    AddWmdmItem(metadata, L"WMDM/FileSize", d->filesize_);

    WMDM_FORMATCODE format;
    switch (d->filetype_) {
      case Type_Aiff:      format = WMDM_FORMATCODE_AIFF; break;
      case Type_Wav:       format = WMDM_FORMATCODE_WAVE; break;
      case Type_Mpeg:      format = WMDM_FORMATCODE_MP3;  break;
      case Type_Asf:       format = WMDM_FORMATCODE_ASF;  break;
      case Type_OggFlac:
      case Type_OggSpeex:
      case Type_OggVorbis: format = WMDM_FORMATCODE_OGG;  break;
      case Type_Mp4:       format = WMDM_FORMATCODE_MP4;  break;
      case Type_Flac:      format = WMDM_FORMATCODE_FLAC; break;
      default:             format = WMDM_FORMATCODE_UNDEFINEDAUDIO; break;
    }
    AddWmdmItem(metadata, L"WMDM/FormatCode", format);
  }
#endif // Q_OS_WIN32

void Song::MergeFromSimpleMetaBundle(const Engine::SimpleMetaBundle &bundle) {
  if (d->init_from_file_) {
    // This Song was already loaded using taglib. Our tags are probably better than the engine's.
    return;
  }

  d->valid_ = true;

  UniversalEncodingHandler detector(NS_FILTER_NON_CJK);
  QTextCodec* codec = detector.Guess(bundle);

  if (!bundle.title.isEmpty()) d->title_ = Decode(bundle.title, codec);
  if (!bundle.artist.isEmpty()) d->artist_ = Decode(bundle.artist, codec);
  if (!bundle.album.isEmpty()) d->album_ = Decode(bundle.album, codec);
  if (!bundle.comment.isEmpty()) d->comment_ = Decode(bundle.comment, codec);
  if (!bundle.genre.isEmpty()) d->genre_ = Decode(bundle.genre, codec);
  if (!bundle.bitrate.isEmpty()) d->bitrate_ = bundle.bitrate.toInt();
  if (!bundle.samplerate.isEmpty()) d->samplerate_ = bundle.samplerate.toInt();
  if (!bundle.length.isEmpty()) set_length_nanosec(bundle.length.toLongLong());
  if (!bundle.year.isEmpty()) d->year_ = bundle.year.toInt();
  if (!bundle.tracknr.isEmpty()) d->track_ = bundle.tracknr.toInt();
}

void Song::BindToQuery(QSqlQuery *query) const {
  #define strval(x) (x.isNull() ? "" : x)
  #define intval(x) (x <= 0 ? -1 : x)
  #define notnullintval(x) (x == -1 ? QVariant() : x)

  // Remember to bind these in the same order as kBindSpec

  query->bindValue(":title", strval(d->title_));
  query->bindValue(":album", strval(d->album_));
  query->bindValue(":artist", strval(d->artist_));
  query->bindValue(":albumartist", strval(d->albumartist_));
  query->bindValue(":composer", strval(d->composer_));
  query->bindValue(":track", intval(d->track_));
  query->bindValue(":disc", intval(d->disc_));
  query->bindValue(":bpm", intval(d->bpm_));
  query->bindValue(":year", intval(d->year_));
  query->bindValue(":genre", strval(d->genre_));
  query->bindValue(":comment", strval(d->comment_));
  query->bindValue(":compilation", d->compilation_ ? 1 : 0);

  query->bindValue(":bitrate", intval(d->bitrate_));
  query->bindValue(":samplerate", intval(d->samplerate_));

  query->bindValue(":directory", notnullintval(d->directory_id_));
  query->bindValue(":filename", d->url_.toEncoded());
  query->bindValue(":mtime", notnullintval(d->mtime_));
  query->bindValue(":ctime", notnullintval(d->ctime_));
  query->bindValue(":filesize", notnullintval(d->filesize_));

  query->bindValue(":sampler", d->sampler_ ? 1 : 0);
  query->bindValue(":art_automatic", d->art_automatic_);
  query->bindValue(":art_manual", d->art_manual_);

  query->bindValue(":filetype", d->filetype_);
  query->bindValue(":playcount", d->playcount_);
  query->bindValue(":lastplayed", intval(d->lastplayed_));
  query->bindValue(":rating", intval(d->rating_));

  query->bindValue(":forced_compilation_on", d->forced_compilation_on_ ? 1 : 0);
  query->bindValue(":forced_compilation_off", d->forced_compilation_off_ ? 1 : 0);

  query->bindValue(":effective_compilation", is_compilation() ? 1 : 0);

  query->bindValue(":skipcount", d->skipcount_);
  query->bindValue(":score", d->score_);

  query->bindValue(":beginning", d->beginning_);
  query->bindValue(":length", intval(length_nanosec()));

  query->bindValue(":cue_path", d->cue_path_);
  query->bindValue(":unavailable", d->unavailable_ ? 1 : 0);
  query->bindValue(":effective_albumartist", this->effective_albumartist());

  #undef intval
  #undef notnullintval
}

void Song::BindToFtsQuery(QSqlQuery *query) const {
  query->bindValue(":ftstitle", d->title_);
  query->bindValue(":ftsalbum", d->album_);
  query->bindValue(":ftsartist", d->artist_);
  query->bindValue(":ftsalbumartist", d->albumartist_);
  query->bindValue(":ftscomposer", d->composer_);
  query->bindValue(":ftsgenre", d->genre_);
  query->bindValue(":ftscomment", d->comment_);
}

#ifdef HAVE_LIBLASTFM
void Song::ToLastFM(lastfm::Track* track, bool prefer_album_artist) const {
  lastfm::MutableTrack mtrack(*track);

  if (prefer_album_artist && !d->albumartist_.isEmpty()) {
    mtrack.setArtist(d->albumartist_);
  } else {
    mtrack.setArtist(d->artist_);
  }
  mtrack.setAlbum(d->album_);
  mtrack.setTitle(d->title_);
  mtrack.setDuration(length_nanosec() / kNsecPerSec);
  mtrack.setTrackNumber(d->track_);

  if (d->filetype_ == Type_Stream && d->end_ == -1) {
    mtrack.setSource(lastfm::Track::NonPersonalisedBroadcast);
  } else {
    mtrack.setSource(lastfm::Track::Player);
  }
}
#endif // HAVE_LIBLASTFM

QString Song::PrettyTitle() const {
  QString title(d->title_);

  if (title.isEmpty())
    title = d->basefilename_;
  if (title.isEmpty())
    title = d->url_.toString();

  return title;
}

QString Song::PrettyTitleWithArtist() const {
  QString title(d->title_);

  if (title.isEmpty())
    title = d->basefilename_;

  if (!d->artist_.isEmpty())
    title = d->artist_ + " - " + title;

  return title;
}

QString Song::PrettyLength() const {
  if (length_nanosec() == -1)
    return QString::null;

  return Utilities::PrettyTimeNanosec(length_nanosec());
}

QString Song::PrettyYear() const {
  if (d->year_ == -1)
    return QString::null;

  return QString::number(d->year_);
}

QString Song::TitleWithCompilationArtist() const {
  QString title(d->title_);

  if (title.isEmpty())
    title = d->basefilename_;

  if (is_compilation() && !d->artist_.isEmpty() && !d->artist_.toLower().contains("various"))
    title = d->artist_ + " - " + title;

  return title;
}

bool Song::IsMetadataEqual(const Song& other) const {
  return d->title_ == other.d->title_ &&
         d->album_ == other.d->album_ &&
         d->artist_ == other.d->artist_ &&
         d->albumartist_ == other.d->albumartist_ &&
         d->composer_ == other.d->composer_ &&
         d->track_ == other.d->track_ &&
         d->disc_ == other.d->disc_ &&
         qFuzzyCompare(d->bpm_, other.d->bpm_) &&
         d->year_ == other.d->year_ &&
         d->genre_ == other.d->genre_ &&
         d->comment_ == other.d->comment_ &&
         d->compilation_ == other.d->compilation_ &&
         d->beginning_ == other.d->beginning_ &&
         length_nanosec() == other.length_nanosec() &&
         d->bitrate_ == other.d->bitrate_ &&
         d->samplerate_ == other.d->samplerate_ &&
         d->art_automatic_ == other.d->art_automatic_ &&
         d->art_manual_ == other.d->art_manual_ &&
         d->cue_path_ == other.d->cue_path_;
}

bool Song::IsEditable() const {
  return d->valid_ && !d->url_.isEmpty() && !is_stream() &&
         d->filetype_ != Type_Unknown && !has_cue();
}

bool Song::operator==(const Song& other) const {
  // TODO: this isn't working for radios
  return url() == other.url() &&
         beginning_nanosec() == other.beginning_nanosec();
}

uint qHash(const Song& song) {
  // Should compare the same fields as operator==
  return qHash(song.url().toString()) ^ qHash(song.beginning_nanosec());
}

bool Song::IsOnSameAlbum(const Song& other) const {
  if (is_compilation() != other.is_compilation())
    return false;

  if (has_cue() && other.has_cue() && cue_path() == other.cue_path())
    return true;

  if (is_compilation() && album() == other.album())
    return true;

  return album() == other.album() && artist() == other.artist();
}

QString Song::AlbumKey() const {
  return QString("%1|%2|%3").arg(
        is_compilation() ? "_compilation" : artist(),
        has_cue() ? cue_path() : "",
        album());
}

void Song::ToXesam(QVariantMap* map) const {
  using mpris::AddMetadata;
  using mpris::AddMetadataAsList;
  using mpris::AsMPRISDateTimeType;

  AddMetadata("xesam:url", url().toString(), map);
  AddMetadata("xesam:title", PrettyTitle(), map);
  AddMetadataAsList("xesam:artist", artist(), map);
  AddMetadata("xesam:album", album(), map);
  AddMetadataAsList("xesam:albumArtist", albumartist(), map);
  AddMetadata("mpris:length", length_nanosec() / kNsecPerUsec, map);
  AddMetadata("xesam:trackNumber", track(), map);
  AddMetadataAsList("xesam:genre", genre(), map);
  AddMetadata("xesam:discNumber", disc(), map);
  AddMetadataAsList("xesam:comment", comment(), map);
  AddMetadata("xesam:contentCreated", AsMPRISDateTimeType(ctime()), map);
  AddMetadata("xesam:lastUsed", AsMPRISDateTimeType(lastplayed()), map);
  AddMetadata("xesam:audioBPM", bpm(), map);
  AddMetadataAsList("xesam:composer", composer(), map);
  AddMetadata("xesam:useCount", playcount(), map);
  AddMetadata("xesam:autoRating", score(), map);
  if (rating() != -1.0) {
    AddMetadata("xesam:userRating", rating(), map);
  }
}
