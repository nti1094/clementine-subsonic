/***************************************************************************
 *   Copyright (C) 2003-2005 by Mark Kretschmann <markey@web.de>           *
 *   Copyright (C) 2005 by Jakub Stachowski <qbast@go2.pl>                 *
 *   Portions Copyright (C) 2006 Paul Cifarelli <paul@cifarelli.net>       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Steet, Fifth Floor, Boston, MA  02111-1307, USA.          *
 ***************************************************************************/

#ifndef AMAROK_GSTENGINE_H
#define AMAROK_GSTENGINE_H

#include "enginebase.h"
#include "bufferconsumer.h"

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimerEvent>

#include <gst/gst.h>
#include <boost/shared_ptr.hpp>

class QTimer;
class QTimerEvent;

class GstEnginePipeline;

/**
 * @class GstEngine
 * @short GStreamer engine plugin
 * @author Mark Kretschmann <markey@web.de>
 */
class GstEngine : public Engine::Base, public BufferConsumer {
  Q_OBJECT

 public:
  GstEngine();
  ~GstEngine();

  struct PluginDetails {
    QString name;
    QString long_name;
    QString author;
    QString description;
  };
  typedef QList<PluginDetails> PluginDetailsList;

  static const char* kSettingsGroup;
  static const char* kAutoSink;

  bool Init();

  bool CanDecode(const QUrl& url);

  int AddBackgroundStream(const QUrl& url);
  void StopBackgroundStream(int id);
  int AllGloryToTheHypnotoad();

  uint position() const;
  uint length() const;
  Engine::State state() const;
  const Engine::Scope& scope();

  PluginDetailsList GetOutputsList() const { return GetPluginList( "Sink/Audio" ); }
  static bool DoesThisSinkSupportChangingTheOutputDeviceToAUserEditableString(const QString& name);

  GstElement* CreateElement(const QString& factoryName, GstElement* bin = 0);

  // BufferConsumer
  void ConsumeBuffer(GstBuffer *buffer, GstEnginePipeline* pipeline);

 public slots:
  void StartPreloading(const QUrl &);
  bool Load(const QUrl&, Engine::TrackChangeType change);
  bool Play(uint offset);
  void Stop();
  void Pause();
  void Unpause();
  void Seek(uint ms);

  /** Set whether equalizer is enabled */
  void SetEqualizerEnabled(bool);

  /** Set equalizer preamp and gains, range -100..100. Gains are 10 values. */
  void SetEqualizerParameters(int preamp, const QList<int>& bandGains);

  void ReloadSettings();

  void AddBufferConsumer(BufferConsumer* consumer);
  void RemoveBufferConsumer(BufferConsumer* consumer);

  void SetSpectrum(bool enable);

 protected:
  void SetVolumeSW(uint percent);
  void timerEvent(QTimerEvent*);

 private slots:
  void EndOfStreamReached(bool has_next_track);
  void HandlePipelineError(const QString& message);
  void NewMetaData(const Engine::SimpleMetaBundle& bundle);
  void ClearScopeBuffers();
  void AddBufferToScope(GstBuffer* buf, GstEnginePipeline* pipeline);
  void FadeoutFinished();
  void SeekNow();
  void BackgroundStreamFinished();
  void PlayDone();

 private:
  // Callbacks
  static void CanDecodeNewPadCallback(GstElement*, GstPad*, gboolean, gpointer);
  static void CanDecodeLastCallback(GstElement*, gpointer);
  static GstBusSyncReply CanDecodeBusCallbackSync(GstBus*, GstMessage*, gpointer);
  static gboolean CanDecodeBusCallback(GstBus*, GstMessage*, gpointer);
  static void PrintGstError(GstMessage* msg);

  static void SetEnv(const char* key, const QString& value);
  PluginDetailsList GetPluginList(const QString& classname) const;

  void StartFadeout();

  void StartTimers();
  void StopTimers();

  boost::shared_ptr<GstEnginePipeline> CreatePipeline();
  boost::shared_ptr<GstEnginePipeline> CreatePipeline(const QUrl& url);

  void UpdateScope();
  qint64 PruneScope();

  int AddBackgroundStream(boost::shared_ptr<GstEnginePipeline> pipeline);

 private:
  static const int kTimerInterval = 1000; // msec
  static const int kPreloadGap = 1000; // msec
  static const int kSeekDelay = 100; // msec

  static const char* kHypnotoadPipeline;

  QString sink_;
  QString device_;

  boost::shared_ptr<GstEnginePipeline> current_pipeline_;
  boost::shared_ptr<GstEnginePipeline> fadeout_pipeline_;
  boost::shared_ptr<GstEnginePipeline> preload_pipeline_;
  QUrl preloaded_url_;

  QList<BufferConsumer*> buffer_consumers_;

  GQueue* delayq_;
  float current_scope_[kScopeSize];
  int current_sample_;

  bool equalizer_enabled_;
  int equalizer_preamp_;
  QList<int> equalizer_gains_;

  bool rg_enabled_;
  int rg_mode_;
  float rg_preamp_;
  bool rg_compression_;

  mutable bool can_decode_success_;
  mutable bool can_decode_last_;

  // Hack to stop seeks happening too often
  QTimer* seek_timer_;
  bool waiting_to_seek_;
  uint seek_pos_;

  int timer_id_;
  int next_element_id_;

  QHash<int, boost::shared_ptr<GstEnginePipeline> > background_streams_;

  bool spectrum_enabled_;
};


#endif /*AMAROK_GSTENGINE_H*/

