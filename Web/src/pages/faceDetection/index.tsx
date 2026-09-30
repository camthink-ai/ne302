import { useEffect, useState, useRef, useCallback } from 'react';
import { useLingui } from '@lingui/react';
import { Label } from '@/components/ui/label';
import { Separator } from '@/components/ui/separator';
import { Switch } from '@/components/ui/switch';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select';
import SvgIcon from '@/components/svg-icon';
import H264Player from '@/lib/MSE/h264Player';
import Loading from '@/components/loading';
import deviceTool from '@/services/api/deviceTool';
import faceDetection, {
  type FaceDetectionConfig,
  type FaceDetectionStats,
  type FaceDetectionResults,
  type FaceTrackInfo,
} from '@/services/api/faceDetection';
import { getWebSocketUrl } from '@/utils';
import { toast } from 'sonner';

const clamp = (v: number, min: number, max: number) => Math.min(max, Math.max(min, v));

/* Sections (tracks card + config panel) share the video's width */
const SECTION_MAX_W = { maxWidth: 'calc(62vh * 16 / 9)' };

const STATUS_CLASS: Record<FaceTrackInfo['recognition']['status'], string> = {
  none: 'bg-gray-500/80 text-white',
  pending: 'bg-sky-600/80 text-white',
  ok: 'bg-green-600/80 text-white',
  unknown: 'bg-amber-600/80 text-white',
  timeout: 'bg-red-600/80 text-white',
  error: 'bg-red-700/80 text-white',
};

export default function FaceDetection() {
  const { i18n } = useLingui();
  const { startVideoStreamReq, stopVideoStreamReq, toggleAiReq } = deviceTool;

  /* ---------- video stream ---------- */
  const playerRef = useRef<HTMLVideoElement>(null);
  const playerRenderRef = useRef<H264Player | null>(null);
  const [playerLoading, setPlayerLoading] = useState(false);

  /* ---------- config / stats / results ---------- */
  const [config, setConfig] = useState<FaceDetectionConfig | null>(null);
  const [stats, setStats] = useState<FaceDetectionStats | null>(null);
  const [results, setResults] = useState<FaceDetectionResults | null>(null);
  const [paramDraft, setParamDraft] = useState<Partial<FaceDetectionConfig>>({});
  const [savingParams, setSavingParams] = useState(false);

  /* ---------- stream lifecycle (mirrors people-counting template) ---------- */
  useEffect(() => {
    let cancelled = false;
    (async () => {
      // The demo consumes the ACTIVE model results and draws its overlay in
      // the camera-frame callback; make sure the AI pipeline is running.
      try {
        await toggleAiReq({ ai_enabled: true });
      } catch (error) {
        console.error(error);
      }
      try {
        setPlayerLoading(true);
        await startVideoStreamReq();
        if (cancelled || !playerRef.current) return;
        const player = new H264Player(() => {});
        playerRenderRef.current = player;
        player.initPlayer(playerRef.current);
        player.start(getWebSocketUrl());
      } catch (error) {
        console.error(error);
      } finally {
        setPlayerLoading(false);
      }
    })();
    return () => {
      cancelled = true;
      playerRenderRef.current?.destroy();
      playerRenderRef.current = null;
      stopVideoStreamReq();
    };
  }, []);

  /* ---------- config load ---------- */
  const loadConfig = useCallback(async () => {
    try {
      const res = await faceDetection.getConfigReq();
      const cfg = res.data as FaceDetectionConfig;
      setConfig(cfg);
      setParamDraft({
        detection_conf_threshold: cfg.detection_conf_threshold,
        min_face_area: cfg.min_face_area,
        track_iou_threshold: cfg.track_iou_threshold,
        track_max_missed: cfg.track_max_missed,
        track_confirm_frames: cfg.track_confirm_frames,
        face_hold_ms: cfg.face_hold_ms,
        mosaic_block: cfg.mosaic_block,
        blur_expand_ratio: cfg.blur_expand_ratio,
        crop_expand_ratio: cfg.crop_expand_ratio,
        crop_min_size: cfg.crop_min_size,
        report_fixed_size: cfg.report_fixed_size,
        jpeg_quality: cfg.jpeg_quality,
        report_confidence_threshold: cfg.report_confidence_threshold,
        report_cooldown_ms: cfg.report_cooldown_ms,
        result_timeout_ms: cfg.result_timeout_ms,
        result_expire_ms: cfg.result_expire_ms,
      });
    } catch (error) {
      console.error(error);
    }
  }, []);

  useEffect(() => {
    loadConfig();
  }, [loadConfig]);

  /* ---------- stats + results polling (1 s) ---------- */
  useEffect(() => {
    let stopped = false;
    const poll = async () => {
      try {
        const [s, r] = await Promise.all([
          faceDetection.getStatusReq(),
          faceDetection.getResultsReq(),
        ]);
        if (!stopped) {
          setStats(s.data as FaceDetectionStats);
          setResults(r.data as FaceDetectionResults);
        }
      } catch {
        /* transient network hiccups are retried by the request layer */
      }
    };
    poll();
    const timer = setInterval(poll, 1000);
    return () => {
      stopped = true;
      clearInterval(timer);
    };
  }, []);

  /* ---------- model upload (active AI slot) ---------- */
  /* ---------- config actions ---------- */
  const patchConfig = async (patch: Partial<FaceDetectionConfig>, successKey?: string) => {
    try {
      await faceDetection.setConfigReq(patch);
      await loadConfig();
      if (successKey) toast.success(i18n._(successKey));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_detection.save_failed'));
    }
  };

  const saveParams = async () => {
    setSavingParams(true);
    try {
      // Merge uncommitted raw text (blur may not have fired before the click)
      const merged = commitTextDrafts();
      if (merged) {
        setParamDraft(merged);
        await patchConfig(merged, 'sys.face_detection.params_saved');
      } else {
        await patchConfig(paramDraft, 'sys.face_detection.params_saved');
      }
    } finally {
      setSavingParams(false);
    }
  };

  const triggerReports = async () => {
    try {
      const res = await faceDetection.triggerReportReq(0);
      const armed = (res.data as { armed: number })?.armed ?? 0;
      toast.success(`${i18n._('sys.face_detection.trigger_done')}: ${armed}`);
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_detection.save_failed'));
    }
  };

  const resetStats = async () => {
    try {
      await faceDetection.resetReq();
      toast.success(i18n._('sys.face_detection.reset_done'));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_detection.save_failed'));
    }
  };

  /* ---------- render helpers ---------- */
  const [textDrafts, setTextDrafts] = useState<Record<string, string>>({});
  const boundsRef = useRef<Record<string, { min: number; max: number }>>({});

  const commitTextDrafts = useCallback(() => {
    const keys = Object.keys(textDrafts);
    if (keys.length === 0) return undefined;
    const merged: Partial<FaceDetectionConfig> = { ...paramDraft };
    for (const key of keys) {
      const bounds = boundsRef.current[key];
      if (!bounds) continue;
      const parsed = Number(textDrafts[key]);
      const v = Number.isFinite(parsed)
        ? clamp(parsed, bounds.min, bounds.max)
        : bounds.min;
      (merged as Record<string, number>)[key] = v;
    }
    setTextDrafts({});
    return merged;
  }, [textDrafts, paramDraft]);

  const numberRow = (
    labelKey: string,
    field: keyof FaceDetectionConfig,
    min: number,
    max: number,
    hintKey?: string,
    step = 1
  ) => {
    boundsRef.current[field] = { min, max };
    const raw = textDrafts[field as string];
    const display = raw !== undefined ? raw : String(paramDraft[field] ?? '');
    return (
      <div className="flex items-center justify-between gap-3 py-1.5">
        <div className="flex flex-col min-w-0">
          <Label className="text-sm shrink-0">{i18n._(labelKey)}</Label>
          {hintKey && <span className="text-xs text-gray-400">{i18n._(hintKey)}</span>}
        </div>
        <Input
          className="w-24 text-right shrink-0"
          type="number"
          min={min}
          max={max}
          step={step}
          value={display}
          onChange={e => {
            setTextDrafts(prev => ({
              ...prev,
              [field]: (e.target as HTMLInputElement).value,
            }));
          }}
          onBlur={() => {
            const merged = commitTextDrafts();
            if (merged) setParamDraft(merged);
          }}
        />
      </div>
    );
  };

  const switchRow = (labelKey: string, field: keyof FaceDetectionConfig, hintKey?: string) => (
    <div className="flex items-center justify-between gap-3 py-1.5">
      <div className="flex flex-col min-w-0">
        <Label className="text-sm shrink-0">{i18n._(labelKey)}</Label>
        {hintKey && <span className="text-xs text-gray-400">{i18n._(hintKey)}</span>}
      </div>
      <Switch
        checked={Boolean(config?.[field])}
        onCheckedChange={checked => patchConfig({ [field]: checked } as Partial<FaceDetectionConfig>)}
      />
    </div>
  );

  const configGroup = (titleKey: string, children: React.ReactNode) => (
    <div className="border border-gray-200 border-solid rounded-md p-3 bg-white">
      <div className="text-sm font-medium text-gray-700 mb-1">{i18n._(titleKey)}</div>
      <div className="flex flex-col">{children}</div>
    </div>
  );

  const trackCard = (t: FaceTrackInfo) => {
    const st = t.recognition?.status ?? 'none';
    const pct = (v?: number) => (v === undefined || v === null ? '--' : `${Math.round(v * 100)}%`);
    let label = i18n._('sys.face_detection.recognizing');
    if (st === 'ok') label = t.recognition.name || i18n._('sys.face_detection.recognized');
    else if (st === 'unknown') label = i18n._('sys.face_detection.unknown');
    else if (st === 'timeout') label = i18n._('sys.face_detection.timeout');
    else if (st === 'error') label = i18n._('sys.face_detection.error');
    else if (st === 'pending') label = i18n._('sys.face_detection.recognizing');
    return (
      <div
        key={t.track_id}
        className="flex items-center justify-between gap-2 border border-gray-200 border-solid rounded-md px-3 py-2 bg-white"
      >
        <div className="flex items-center gap-2 min-w-0">
          <span className="font-mono font-bold text-gray-700">#{t.track_id}</span>
          <span className="text-xs text-gray-500">{pct(t.detection_confidence)}</span>
        </div>
        <div className="flex items-center gap-2 min-w-0">
          <span
            className={`px-2 py-0.5 rounded text-xs text-white truncate max-w-[160px] ${STATUS_CLASS[st]}`}
            title={t.recognition?.name}
          >
            {label}
          </span>
          {st === 'ok' && (
            <span className="text-xs text-gray-600">{pct(t.recognition.confidence)}</span>
          )}
        </div>
      </div>
    );
  };

  /* ---------- one-shot recognition verification ---------- */
  const verifyFileRef = useRef<HTMLInputElement>(null);
  const [verifying, setVerifying] = useState(false);
  const [verifyResult, setVerifyResult] = useState<
    { status: string; name: string; confidence: number } | null
  >(null);

  const runVerify = async (file: File) => {
    setVerifying(true);
    setVerifyResult(null);
    try {
      const res = await faceDetection.verifyReq(file);
      setVerifyResult(res.data as { status: string; name: string; confidence: number });
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.face_detection.save_failed'));
    } finally {
      setVerifying(false);
    }
  };

  const [configSectionOpen, setConfigSectionOpen] = useState(true);

  return (
    <div className="flex flex-col gap-3 mt-4 bg-gray-100 p-4 rounded-lg">
      {/* video: demo OSD (mosaic/boxes/labels) is burned into the stream */}
      <div
        className="relative bg-black w-full aspect-video mx-auto overflow-hidden"
        style={{ maxWidth: 'calc(62vh * 16 / 9)' }}
      >
        <video
          ref={playerRef as React.Ref<HTMLVideoElement>}
          id="face-player"
          className="w-full h-full"
          muted
          playsInline
          disableRemotePlayback
        />
        {playerLoading && (
          <div className="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2">
            <Loading placeholder="Loading..." />
          </div>
        )}
        {/* stats overlay (top-right): key counters only */}
        {stats && (
          <div className="absolute right-3 top-3 px-3 py-1.5 rounded bg-black/60 text-white text-xs font-medium backdrop-blur-sm">
            {i18n._('sys.face_detection.st_now')} {stats.confirmed_tracks}
            {' · '}
            {i18n._('sys.face_detection.st_total')} {stats.total_tracks}
            {' · '}
            <span className="text-green-300">
              {i18n._('sys.face_detection.st_ok')} {stats.results_ok}
            </span>
            {' · '}
            <span className="text-amber-300">
              {i18n._('sys.face_detection.st_unk')} {stats.results_unknown}
            </span>
            {' · '}
            <span className="text-red-300">
              {i18n._('sys.face_detection.st_to')} {stats.results_timeout}
            </span>
            {' · '}
            {i18n._('sys.face_detection.st_rep')} {stats.reports_sent}
          </div>
        )}
        {/* active model badge */}
        {stats && (
          <div
            className={`absolute right-3 bottom-3 px-2 py-1 rounded text-xs max-w-[60%] truncate ${
              stats.model ? 'bg-green-600/80 text-white' : 'bg-orange-600/80 text-white'
            }`}
            title={stats.model?.name}
          >
            {stats.model
              ? stats.model.name
              : i18n._('sys.face_detection.model_missing')}
          </div>
        )}
      </div>

      {/* live tracks + actions, one card, same width as the video */}
      <div className="w-full mx-auto flex flex-col gap-2" style={SECTION_MAX_W}>
        <div className="border border-gray-200 border-solid rounded-md bg-white p-3 flex flex-col gap-3">
          <div className="flex items-center justify-between gap-2">
            <Label>{i18n._('sys.face_detection.tracks_title')}</Label>
            <div className="flex gap-2">
              <Button type="button" variant="outline" size="sm" onClick={resetStats}>
                {i18n._('sys.face_detection.reset')}
              </Button>
              <Button type="button" variant="primary" size="sm" onClick={triggerReports}>
                {i18n._('sys.face_detection.trigger')}
              </Button>
            </div>
          </div>
          {results && results.tracks.length > 0 ? (
            <div className="grid gap-2 md:grid-cols-2">
              {results.tracks.map(trackCard)}
            </div>
          ) : (
            <div className="border border-dashed border-gray-300 border-solid rounded-md p-4 text-center text-sm text-gray-400">
              {i18n._('sys.face_detection.no_tracks')}
            </div>
          )}
        </div>
      </div>

      {/* one-shot recognition verification (upload a face photo) */}
      <div className="w-full mx-auto flex flex-col gap-2" style={SECTION_MAX_W}>
        <div className="border border-gray-200 border-solid rounded-md bg-white p-3 flex flex-col gap-3">
          <div className="flex items-center justify-between gap-2">
            <Label>{i18n._('sys.face_detection.verify_title')}</Label>
            <input
              ref={verifyFileRef}
              type="file"
              accept="image/jpeg,image/png"
              className="hidden"
              onChange={async ev => {
                const inputEl = ev.target as HTMLInputElement;
                const file = inputEl.files?.[0];
                inputEl.value = '';
                if (file) await runVerify(file);
              }}
            />
            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={verifying}
              onClick={() => verifyFileRef.current?.click()}
            >
              {verifying ? i18n._('sys.face_detection.verify_running') : i18n._('common.upload')}
            </Button>
          </div>
          <span className="text-xs text-gray-400">{i18n._('sys.face_detection.verify_hint')}</span>
          {verifyResult && (
            <div
              className={`rounded-md px-3 py-2 text-sm border border-solid ${
                verifyResult.status === 'ok'
                  ? 'border-green-300 bg-green-50 text-green-700'
                  : verifyResult.status === 'error'
                    ? 'border-red-300 bg-red-50 text-red-700'
                    : 'border-amber-300 bg-amber-50 text-amber-700'
              }`}
            >
              {verifyResult.status === 'ok'
                ? `${verifyResult.name} · ${Math.round(verifyResult.confidence * 100)}%`
                : verifyResult.status === 'unknown'
                  ? i18n._('sys.face_detection.unknown')
                  : i18n._('sys.face_detection.error')}
            </div>
          )}
        </div>
      </div>

      <Separator />

      {/* config panel, same width as the video */}
      <div className="w-full mx-auto flex flex-col gap-2" style={SECTION_MAX_W}>
        <button
          type="button"
          className="flex w-full items-center justify-between text-left"
          onClick={() => setConfigSectionOpen(o => !o)}
        >
          <Label>{i18n._('sys.face_detection.config_title')}</Label>
          <span className="inline-flex h-4 w-4 shrink-0 items-center justify-center text-gray-500">
            <SvgIcon
              icon="right"
              className={`h-4 w-4 transition-transform duration-200 ${
                configSectionOpen ? 'rotate-90' : 'rotate-0'
              }`}
            />
          </span>
        </button>
        {configSectionOpen && (
          <div className="flex flex-col gap-3">
            {configGroup(
'sys.face_detection.g_basic',
              switchRow('sys.face_detection.enabled', 'enabled', 'sys.face_detection.enabled_hint')
)}
            {configGroup(
'sys.face_detection.g_display',
              <>
                {switchRow('sys.face_detection.show_box', 'show_box')}
                {switchRow('sys.face_detection.show_id', 'show_id')}
                {switchRow('sys.face_detection.show_confidence', 'show_confidence')}
                {switchRow('sys.face_detection.show_landmarks', 'show_landmarks', 'sys.face_detection.show_landmarks_hint')}
                {switchRow('sys.face_detection.show_recognition', 'show_recognition')}
              </>
)}
            {configGroup(
'sys.face_detection.g_mosaic',
              <>
                {switchRow('sys.face_detection.blur_enabled', 'blur_enabled')}
                {numberRow('sys.face_detection.mosaic_block', 'mosaic_block', 4, 64)}
                {numberRow('sys.face_detection.blur_expand_ratio', 'blur_expand_ratio', 0, 1, undefined, 0.01)}
              </>
)}
            {configGroup(
'sys.face_detection.g_report',
              <>
                {switchRow('sys.face_detection.report_enabled', 'report_enabled')}
                <div className="flex items-center justify-between gap-3 py-1.5">
                  <div className="flex flex-col min-w-0">
                    <Label className="text-sm shrink-0">{i18n._('sys.face_detection.report_channel')}</Label>
                  </div>
                  <Select
                    value={String(config?.report_channel ?? 0)}
                    onValueChange={v => patchConfig({ report_channel: Number(v) })}
                  >
                    <SelectTrigger className="w-36 shrink-0">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="0">Webhook</SelectItem>
                      <SelectItem value="1">MQTT</SelectItem>
                    </SelectContent>
                  </Select>
                </div>
                {switchRow('sys.face_detection.report_periodic', 'report_periodic_enabled', 'sys.face_detection.report_periodic_hint')}
                {numberRow('sys.face_detection.report_conf', 'report_confidence_threshold', 0.01, 0.99, undefined, 0.01)}
                {numberRow('sys.face_detection.report_cooldown_ms', 'report_cooldown_ms', 100, 3600000)}
                {numberRow('sys.face_detection.result_timeout_ms', 'result_timeout_ms', 100, 60000)}
                {numberRow('sys.face_detection.result_expire_ms', 'result_expire_ms', 1000, 3600000)}
              </>
)}
            {configGroup(
'sys.face_detection.g_crop',
              <>
                {numberRow('sys.face_detection.crop_expand_ratio', 'crop_expand_ratio', 0, 1, 'sys.face_detection.crop_expand_hint', 0.01)}
                {numberRow('sys.face_detection.crop_min_size', 'crop_min_size', 8, 512)}
                {numberRow('sys.face_detection.report_fixed_size', 'report_fixed_size', 0, 512, 'sys.face_detection.fixed_size_hint')}
                {numberRow('sys.face_detection.jpeg_quality', 'jpeg_quality', 10, 95)}
              </>
)}
            {configGroup(
'sys.face_detection.g_track',
              <>
                {numberRow('sys.face_detection.detection_conf', 'detection_conf_threshold', 0.01, 0.99, 'sys.face_detection.detection_conf_hint', 0.01)}
                {numberRow('sys.face_detection.min_face_area', 'min_face_area', 0, 1000000, 'sys.face_detection.min_face_area_hint')}
                {numberRow('sys.face_detection.track_iou', 'track_iou_threshold', 0.01, 0.99, undefined, 0.01)}
                {numberRow('sys.face_detection.track_max_missed', 'track_max_missed', 1, 50)}
                {numberRow('sys.face_detection.track_confirm_frames', 'track_confirm_frames', 1, 20)}
                {numberRow('sys.face_detection.face_hold_ms', 'face_hold_ms', 100, 2000)}
                {switchRow('sys.face_detection.extrapolation', 'extrapolation_enabled', 'sys.face_detection.extrapolation_hint')}
              </>
)}
            <div className="flex justify-end">
              <Button type="button" variant="primary" size="sm" disabled={savingParams} onClick={saveParams}>
                {i18n._('common.save')}
              </Button>
            </div>
          </div>
        )}
      </div>

    </div>
  );
}
