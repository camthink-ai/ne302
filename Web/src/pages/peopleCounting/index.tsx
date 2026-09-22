import { useEffect, useState, useRef, useCallback } from 'react';
import { useLingui } from '@lingui/react';
import { Label } from '@/components/ui/label';
import { Separator } from '@/components/ui/separator';
import { Switch } from '@/components/ui/switch';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import SvgIcon from '@/components/svg-icon';
import H264Player from '@/lib/MSE/h264Player';
import Loading from '@/components/loading';
import deviceTool from '@/services/api/deviceTool';
import systemApis from '@/services/api/system';
import peopleCounting, { type PeopleCountingConfig, type PeopleCountingStats } from '@/services/api/peopleCounting';
import request from '@/services/request';
import { getWebSocketUrl } from '@/utils';
import { toast } from 'sonner';

interface LineDraft {
  x1: number;
  y1: number;
  x2: number;
  y2: number;
}

const clamp = (v: number, min: number, max: number) => Math.min(max, Math.max(min, v));

export default function PeopleCounting() {
  const { i18n } = useLingui();
  const { startVideoStreamReq, stopVideoStreamReq, toggleAiReq } = deviceTool;
  const { preCheckReq, uploadOTAFileReq, reloadModelReq } = systemApis;

  /* ---------- video stream ---------- */
  const playerRef = useRef<HTMLVideoElement>(null);
  const playerRenderRef = useRef<H264Player | null>(null);
  const [playerLoading, setPlayerLoading] = useState(false);

  /* ---------- config / stats ---------- */
  const [config, setConfig] = useState<PeopleCountingConfig | null>(null);
  const [stats, setStats] = useState<PeopleCountingStats | null>(null);
  const [paramDraft, setParamDraft] = useState<Partial<PeopleCountingConfig>>({});
  const [savingParams, setSavingParams] = useState(false);

  /* ---------- counting line canvas ---------- */
  const wrapRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [editLine, setEditLine] = useState(false);
  const [lineDraft, setLineDraft] = useState<LineDraft | null>(null);
  const dragIdxRef = useRef<0 | 1 | null>(null);

  /* ---------- stream lifecycle (mirrors hardwareManagement/graphics.tsx) ---------- */
  useEffect(() => {
    let cancelled = false;
    (async () => {
      // The counting demo needs the AI pipeline running and on-frame drawing
      // enabled so the camera draw callback renders line/OSD/mosaic.
      try {
        await toggleAiReq({ ai_enabled: true });
        await request.post('/api/v1/ai/params', { overlay_results: true });
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
      const res = await peopleCounting.getCountingConfigReq();
      const cfg = res.data as PeopleCountingConfig;
      setConfig(cfg);
      setParamDraft({
        line_thickness: cfg.line_thickness,
        track_iou_threshold: cfg.track_iou_threshold,
        track_max_missed: cfg.track_max_missed,
        cross_debounce: cfg.cross_debounce,
        min_box_area: cfg.min_box_area,
        mosaic_block: cfg.mosaic_block,
        mosaic_expand: cfg.mosaic_expand,
        face_conf_threshold: cfg.face_conf_threshold,
        face_nms_threshold: cfg.face_nms_threshold,
      });
    } catch (error) {
      console.error(error);
    }
  }, []);

  useEffect(() => {
    loadConfig();
  }, [loadConfig]);

  /* ---------- stats polling (1 s) ---------- */
  useEffect(() => {
    let stopped = false;
    const poll = async () => {
      try {
        const res = await peopleCounting.getCountingStatsReq();
        if (!stopped) setStats(res.data as PeopleCountingStats);
      } catch {
        /* transient Wi-Fi hiccups are retried by the request layer */
      }
    };
    poll();
    const timer = setInterval(poll, 1000);
    return () => {
      stopped = true;
      clearInterval(timer);
    };
  }, []);

  /* ---------- canvas drawing ---------- */
  const drawOverlay = useCallback(() => {
    const canvas = canvasRef.current;
    const wrap = wrapRef.current;
    if (!canvas || !wrap) return;
    const w = wrap.clientWidth;
    const h = wrap.clientHeight;
    if (w === 0 || h === 0) return;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    const source = lineDraft ?? (config
      ? { x1: config.line_x1, y1: config.line_y1, x2: config.line_x2, y2: config.line_y2 }
      : null);
    // The firmware already burns the counting line + direction arrow into
    // the encoded video when show_line is on, so the canvas overlay only
    // draws during editing (as the draggable draft) to avoid a doubled line
    // and two arrows in normal mode.
    if (!source || !editLine) return;

    const x1 = (source.x1 / 1000) * w;
    const y1 = (source.y1 / 1000) * h;
    const x2 = (source.x2 / 1000) * w;
    const y2 = (source.y2 / 1000) * h;

    // main line (yellow, matching firmware COLOR_YELLOW)
    ctx.strokeStyle = '#FFEB00';
    ctx.lineWidth = Math.max(2, config?.line_thickness ?? 3);
    ctx.lineCap = 'round';
    ctx.beginPath();
    ctx.moveTo(x1, y1);
    ctx.lineTo(x2, y2);
    ctx.stroke();

    // direction arrow toward the "in" side (same math as firmware:
    // positive-side normal of P1->P2 is (-dy, dx))
    let dx = x2 - x1;
    let dy = y2 - y1;
    const len = Math.hypot(dx, dy);
    if (len > 8) {
      dx /= len;
      dy /= len;
      const nx = -dy;
      const ny = dx;
      const sign = config?.invert_direction ? -1 : 1;
      const mx = (x1 + x2) / 2;
      const my = (y1 + y2) / 2;
      const arrowLen = Math.min(48, len * 0.3);
      const head = arrowLen * 0.35;
      const tipX = mx + nx * sign * arrowLen;
      const tipY = my + ny * sign * arrowLen;
      ctx.strokeStyle = '#22DD66';
      ctx.lineWidth = Math.max(2, config?.line_thickness ?? 3);
      // shaft
      ctx.beginPath();
      ctx.moveTo(mx, my);
      ctx.lineTo(tipX, tipY);
      // head barbs: back along the shaft, flare sideways along the line
      const backX = tipX - nx * sign * head;
      const backY = tipY - ny * sign * head;
      ctx.moveTo(tipX, tipY);
      ctx.lineTo(backX + dx * head, backY + dy * head);
      ctx.moveTo(tipX, tipY);
      ctx.lineTo(backX - dx * head, backY - dy * head);
      ctx.stroke();
    }

    // endpoints
    for (const [px, py] of [[x1, y1], [x2, y2]] as const) {
      ctx.beginPath();
      ctx.arc(px, py, editLine ? 9 : 5, 0, Math.PI * 2);
      ctx.fillStyle = editLine ? '#FFEB00' : 'rgba(255,235,0,0.55)';
      ctx.fill();
      ctx.lineWidth = 2;
      ctx.strokeStyle = '#00000088';
      ctx.stroke();
    }
  }, [config, lineDraft, editLine]);

  useEffect(() => {
    drawOverlay();
  }, [drawOverlay]);

  useEffect(() => {
    const wrap = wrapRef.current;
    if (!wrap) return undefined;
    const observer = new ResizeObserver(() => drawOverlay());
    observer.observe(wrap);
    return () => observer.disconnect();
  }, [drawOverlay]);

  /* ---------- line dragging ---------- */
  const pointToPermille = (e: React.PointerEvent<HTMLCanvasElement>) => {
    const canvas = canvasRef.current;
    if (!canvas) return null;
    const rect = canvas.getBoundingClientRect();
    return {
      x: clamp(Math.round(((e.clientX - rect.left) / rect.width) * 1000), 0, 1000),
      y: clamp(Math.round(((e.clientY - rect.top) / rect.height) * 1000), 0, 1000),
    };
  };

  const handlePointerDown = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!editLine) return;
    const canvas = canvasRef.current;
    const wrap = wrapRef.current;
    if (!canvas || !wrap) return;
    const rect = canvas.getBoundingClientRect();
    const w = rect.width;
    const h = rect.height;
    const source = lineDraft ?? (config
      ? { x1: config.line_x1, y1: config.line_y1, x2: config.line_x2, y2: config.line_y2 }
      : null);
    if (!source) return;
    const px = ((e.clientX - rect.left) / w) * 1000;
    const py = ((e.clientY - rect.top) / h) * 1000;
    const d1 = Math.hypot(px - source.x1, py - source.y1);
    const d2 = Math.hypot(px - source.x2, py - source.y2);
    const threshold = Math.max(60, (24 / w) * 1000);
    if (d1 < threshold && d1 <= d2) dragIdxRef.current = 0;
    else if (d2 < threshold) dragIdxRef.current = 1;
    else return;
    canvas.setPointerCapture(e.pointerId);
  };

  const handlePointerMove = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (!editLine || dragIdxRef.current === null) return;
    const p = pointToPermille(e);
    if (!p) return;
    const source = lineDraft ?? (config
      ? { x1: config.line_x1, y1: config.line_y1, x2: config.line_x2, y2: config.line_y2 }
      : null);
    if (!source) return;
    if (dragIdxRef.current === 0) {
      setLineDraft({ ...source, x1: p.x, y1: p.y });
    } else {
      setLineDraft({ ...source, x2: p.x, y2: p.y });
    }
  };

  const handlePointerUp = (e: React.PointerEvent<HTMLCanvasElement>) => {
    if (dragIdxRef.current === null) return;
    dragIdxRef.current = null;
    try {
      canvasRef.current?.releasePointerCapture(e.pointerId);
    } catch {
      /* pointer already released */
    }
  };

  /* ---------- model upload ---------- */
  const personFileRef = useRef<HTMLInputElement>(null);
  const faceFileRef = useRef<HTMLInputElement>(null);
  const [personUpload, setPersonUpload] = useState({ loading: false, pct: 0 });
  const [faceUpload, setFaceUpload] = useState({ loading: false, pct: 0 });

  const handleModelUpload = async (file: File, kind: 'person' | 'face') => {
    const isFace = kind === 'face';
    const setUpload = isFace ? setFaceUpload : setPersonUpload;
    try {
      setUpload({ loading: true, pct: 0 });
      await stopVideoStreamReq();
      const preview = file.slice(0, 2048);
      await preCheckReq(preview, isFace ? 'ai_face' : 'ai');
      await uploadOTAFileReq(file, isFace ? 'ai_face' : 'ai', {
        onUploadProgress: (e: { loaded: number; total?: number }) => {
          if (e.total) {
            setUpload({ loading: true, pct: Math.round((e.loaded / e.total) * 100) });
          }
        },
      });
      if (isFace) {
        await peopleCounting.reloadFaceModelReq();
      } else {
        await reloadModelReq();
      }
      toast.success(i18n._('sys.people_counting.model_upload_ok'));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.people_counting.model_upload_failed'));
    } finally {
      setUpload({ loading: false, pct: 0 });
      try {
        await new Promise(resolve => { setTimeout(resolve, 1000); });
        await startVideoStreamReq();
      } catch (restartError) {
        console.error('preview restart after model upload', restartError);
      }
    }
  };

  /* ---------- config actions ---------- */
  const patchConfig = async (patch: Partial<PeopleCountingConfig>, successKey?: string) => {
    try {
      await peopleCounting.setCountingConfigReq(patch);
      await loadConfig();
      if (successKey) toast.success(i18n._(successKey));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.people_counting.save_failed'));
    }
  };

  const saveLine = async () => {
    if (!lineDraft) return;
    await patchConfig(
      { line_x1: lineDraft.x1, line_y1: lineDraft.y1, line_x2: lineDraft.x2, line_y2: lineDraft.y2 },
      'sys.people_counting.line_saved'
    );
    setEditLine(false);
  };

  const saveParams = async () => {
    setSavingParams(true);
    try {
      await patchConfig(paramDraft, 'sys.people_counting.params_saved');
    } finally {
      setSavingParams(false);
    }
  };

  const resetStats = async () => {
    try {
      await peopleCounting.resetCountingReq();
      const res = await peopleCounting.getCountingStatsReq();
      setStats(res.data as PeopleCountingStats);
      toast.success(i18n._('sys.people_counting.reset_done'));
    } catch (error) {
      console.error(error);
      toast.error(i18n._('sys.people_counting.save_failed'));
    }
  };

  /* ---------- render helpers ---------- */
  const statCard = (labelKey: string, value: number | string | undefined, accent = '') => (
    <div className="flex-1 min-w-[120px] border border-gray-200 border-solid rounded-lg p-4 flex flex-col items-center gap-1 bg-white">
      <span className="text-sm text-gray-500">{i18n._(labelKey)}</span>
      <span className={`text-3xl font-bold ${accent}`}>{value ?? '--'}</span>
    </div>
  );

  const lineInputs = lineDraft
    ? ([['x1', 'line_x1'], ['y1', 'line_y1'], ['x2', 'line_x2'], ['y2', 'line_y2']] as const).map(
        ([key, label]) => (
          <div key={key} className="flex flex-col gap-1 items-center">
            <Label className="text-xs text-gray-500">
              {i18n._(`sys.people_counting.${label}`)}
            </Label>
            <Input
              className="w-20 text-right"
              type="number"
              min={0}
              max={1000}
              value={lineDraft[key as keyof LineDraft]}
              onChange={e => {
                const v = clamp(Number((e.target as HTMLInputElement).value || 0), 0, 1000);
                setLineDraft(prev => (prev ? { ...prev, [key]: v } : prev));
              }}
            />
          </div>
        )
      )
    : null;

  const numberRow = (
    labelKey: string,
    field: keyof PeopleCountingConfig,
    min: number,
    max: number,
    hintKey?: string
  ) => (
    <div className="flex justify-between gap-4 items-center">
      <div className="flex min-w-0 items-center gap-2">
        <Label className="shrink-0">{i18n._(labelKey)}</Label>
        {hintKey && (
          <span className="text-xs text-gray-400 truncate">{i18n._(hintKey)}</span>
        )}
      </div>
      <Input
        className="w-24 text-right"
        type="number"
        min={min}
        max={max}
        value={paramDraft[field] as number | undefined}
        onChange={e => {
          const v = clamp(Number((e.target as HTMLInputElement).value || 0), min, max);
          setParamDraft(prev => ({ ...prev, [field]: v }));
        }}
      />
    </div>
  );

  const switchRow = (
    labelKey: string,
    field: keyof PeopleCountingConfig,
    hintKey?: string
  ) => (
    <div className="flex justify-between gap-4 items-center">
      <div className="flex min-w-0 items-center gap-2">
        <Label className="shrink-0">{i18n._(labelKey)}</Label>
        {hintKey && (
          <span className="text-xs text-gray-400 truncate">{i18n._(hintKey)}</span>
        )}
      </div>
      <Switch
        checked={Boolean(config?.[field])}
        onCheckedChange={checked => patchConfig({ [field]: checked } as Partial<PeopleCountingConfig>)}
      />
    </div>
  );

  const [configSectionOpen, setConfigSectionOpen] = useState(true);

  return (
    <div className="flex flex-col gap-3 mt-4 bg-gray-100 p-4 rounded-lg">
      {/* video + line overlay: cap the height so the whole video stays in
          view; width follows 16:9 of the capped height (canvas maps permille
          coordinates over this same box, so it must stay aspect-locked) */}
      <div
        ref={wrapRef}
        className="relative bg-black w-full aspect-video mx-auto overflow-hidden"
        style={{ maxWidth: 'calc(62vh * 16 / 9)' }}
      >
        <video
          ref={playerRef as React.Ref<HTMLVideoElement>}
          id="counting-player"
          className="w-full h-full"
          muted
          playsInline
          disableRemotePlayback
        />
        <canvas
          ref={canvasRef}
          className={`absolute inset-0 w-full h-full ${editLine ? 'cursor-crosshair touch-none' : 'pointer-events-none'}`}
          onPointerDown={handlePointerDown}
          onPointerMove={handlePointerMove}
          onPointerUp={handlePointerUp}
          onPointerCancel={handlePointerUp}
        />
        {playerLoading && (
          <div className="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2">
            <Loading placeholder="Loading..." />
          </div>
        )}
        {/* edit toolbar */}
        <div className="absolute left-3 bottom-3 flex gap-2">
          {!editLine ? (
            <Button
              type="button"
              size="sm"
              variant="outline"
              className="bg-black/60 text-white border-white/30 hover:bg-black/80"
              onClick={() => setEditLine(true)}
            >
              {i18n._('sys.people_counting.edit_line')}
            </Button>
          ) : (
            <>
              <Button
                type="button"
                size="sm"
                className="bg-primary text-white"
                onClick={saveLine}
              >
                {i18n._('sys.people_counting.save_line')}
              </Button>
              <Button
                type="button"
                size="sm"
                variant="outline"
                className="bg-black/60 text-white border-white/30 hover:bg-black/80"
                onClick={() => {
                  setLineDraft(null);
                  setEditLine(false);
                }}
              >
                {i18n._('common.cancel')}
              </Button>
            </>
          )}
        </div>
        {/* face model badge */}
        {stats && (
          <div
            className={`absolute right-3 bottom-3 px-2 py-1 rounded text-xs ${
              stats.face_model_loaded
                ? 'bg-green-600/80 text-white'
                : 'bg-orange-600/80 text-white'
            }`}
          >
            {stats.face_model_loaded
              ? i18n._('sys.people_counting.face_model_ok')
              : i18n._('sys.people_counting.face_model_missing')}
          </div>
        )}
      </div>

      {/* line numeric inputs while editing */}
      {editLine && (
        <div className="flex flex-wrap gap-4 justify-center items-end border border-gray-200 border-solid p-3 rounded-md bg-white">
          {lineInputs}
        </div>
      )}

      {/* stats cards */}
      <div className="flex flex-wrap gap-3">
        {statCard('sys.people_counting.current_count', stats?.current_count, 'text-blue-600')}
        {statCard('sys.people_counting.in_count', stats?.in_count, 'text-green-600')}
        {statCard('sys.people_counting.out_count', stats?.out_count, 'text-orange-600')}
        {statCard('sys.people_counting.net_count', stats?.net_count)}
      </div>

      {/* actions */}
      <div className="flex justify-end gap-2">
        <Button type="button" variant="outline" size="sm" onClick={resetStats}>
          {i18n._('sys.people_counting.reset')}
        </Button>
      </div>

      <Separator />

      {/* config panel */}
      <div className="flex flex-col gap-2">
        <button
          type="button"
          className="flex w-full items-center justify-between text-left"
          onClick={() => setConfigSectionOpen(o => !o)}
        >
          <Label>{i18n._('sys.people_counting.config_title')}</Label>
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
          <div className="border border-gray-200 border-solid p-4 rounded-md flex flex-col gap-3 bg-white">
            {switchRow('sys.people_counting.enabled', 'enabled', 'sys.people_counting.enabled_hint')}
            <Separator />
            {switchRow('sys.people_counting.show_line', 'show_line')}
            {switchRow('sys.people_counting.show_osd', 'show_osd')}
            {switchRow('sys.people_counting.invert_direction', 'invert_direction', 'sys.people_counting.invert_direction_hint')}
            <Separator />
            {switchRow('sys.people_counting.face_mosaic', 'face_mosaic')}
            {numberRow('sys.people_counting.mosaic_block', 'mosaic_block', 4, 64, 'sys.people_counting.mosaic_block_hint')}
            {numberRow('sys.people_counting.mosaic_expand', 'mosaic_expand', 0, 100, 'sys.people_counting.mosaic_expand_hint')}
            {numberRow('sys.people_counting.face_conf', 'face_conf_threshold', 1, 99, 'sys.people_counting.face_conf_hint')}
            {numberRow('sys.people_counting.face_nms', 'face_nms_threshold', 1, 99, 'sys.people_counting.face_nms_hint')}
            <Separator />
            {numberRow('sys.people_counting.track_iou', 'track_iou_threshold', 1, 99, 'sys.people_counting.track_iou_hint')}
            {numberRow('sys.people_counting.track_max_missed', 'track_max_missed', 1, 50, 'sys.people_counting.track_max_missed_hint')}
            {numberRow('sys.people_counting.cross_debounce', 'cross_debounce', 1, 10, 'sys.people_counting.cross_debounce_hint')}
            {numberRow('sys.people_counting.min_box_area', 'min_box_area', 0, 1000000, 'sys.people_counting.min_box_area_hint')}
            {numberRow('sys.people_counting.line_thickness', 'line_thickness', 1, 16)}
            <div className="flex justify-end">
              <Button type="button" variant="primary" size="sm" disabled={savingParams} onClick={saveParams}>
                {i18n._('common.save')}
              </Button>
            </div>
          </div>
        )}
      </div>

      <Separator />

      {/* model upload */}
      <div className="flex flex-col gap-2">
        <Label>{i18n._('sys.people_counting.model_section')}</Label>
        <div className="grid gap-3 md:grid-cols-2">
          <div className="border border-gray-200 border-solid p-4 rounded-md bg-white flex flex-col gap-2">
            <Label>{i18n._('sys.people_counting.upload_person')}</Label>
            <span className="text-xs text-gray-400">{i18n._('sys.people_counting.person_hint')}</span>
            <input
              ref={personFileRef}
              type="file"
              accept=".bin"
              className="hidden"
              onChange={async ev => {
                const inputEl = ev.target as HTMLInputElement;
                const file = inputEl.files?.[0];
                inputEl.value = '';
                if (file) await handleModelUpload(file, 'person');
              }}
            />
            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={personUpload.loading || faceUpload.loading}
              onClick={() => personFileRef.current?.click()}
            >
              {personUpload.loading
                ? `${i18n._('sys.people_counting.uploading')} ${personUpload.pct}%`
                : i18n._('common.upload')}
            </Button>
          </div>
          <div className="border border-gray-200 border-solid p-4 rounded-md bg-white flex flex-col gap-2">
            <Label>{i18n._('sys.people_counting.upload_face')}</Label>
            <span className="text-xs text-gray-400">{i18n._('sys.people_counting.face_hint')}</span>
            <input
              ref={faceFileRef}
              type="file"
              accept=".bin"
              className="hidden"
              onChange={async ev => {
                const inputEl = ev.target as HTMLInputElement;
                const file = inputEl.files?.[0];
                inputEl.value = '';
                if (file) await handleModelUpload(file, 'face');
              }}
            />
            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={personUpload.loading || faceUpload.loading}
              onClick={() => faceFileRef.current?.click()}
            >
              {faceUpload.loading
                ? `${i18n._('sys.people_counting.uploading')} ${faceUpload.pct}%`
                : i18n._('common.upload')}
            </Button>
          </div>
        </div>
      </div>
    </div>
  );
}
