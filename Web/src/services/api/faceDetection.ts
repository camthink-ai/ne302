import request from '../request';

/** Face detection demo configuration, exactly as the firmware API exposes it
 * (see Custom/Core/Video/face_detection_config.h and Docs/Face_Detection_Demo.md
 * section 4). Ratios/thresholds are floats; everything else is integral. */
export interface FaceDetectionConfig {
  enabled: boolean;
  detection_conf_threshold: number;
  min_face_area: number;
  track_iou_threshold: number;
  track_max_missed: number;
  track_confirm_frames: number;
  face_hold_ms: number;
  extrapolation_enabled: boolean;
  blur_enabled: boolean;
  mosaic_block: number;
  blur_expand_ratio: number;
  crop_expand_ratio: number;
  crop_min_size: number;
  report_fixed_size: number;
  jpeg_quality: number;
  report_enabled: boolean;
  report_channel: number;
  report_confidence_threshold: number;
  report_periodic_enabled: boolean;
  report_cooldown_ms: number;
  result_timeout_ms: number;
  result_expire_ms: number;
  show_box: boolean;
  show_id: boolean;
  show_confidence: boolean;
  show_landmarks: boolean;
  show_recognition: boolean;
}

export interface FaceDetectionModelInfo {
  name: string;
  input_width: number;
  input_height: number;
  postprocess_type: string;
}

export interface FaceDetectionStats {
  enabled: boolean;
  confirmed_tracks: number;
  total_tracks: number;
  last_detections: number;
  reports_queued: number;
  reports_sent: number;
  reports_dropped: number;
  reports_failed: number;
  results_ok: number;
  results_unknown: number;
  results_timeout: number;
  last_update_ms: number;
  model?: FaceDetectionModelInfo;
}

export interface FaceTrackInfo {
  track_id: number;
  bbox: [number, number, number, number];
  detection_confidence: number;
  nb_landmarks?: number;
  landmarks: number[][] | null;
  recognition: {
    status: 'none' | 'pending' | 'ok' | 'unknown' | 'timeout' | 'error';
    name: string;
    confidence: number;
  };
}

export interface FaceDetectionResults {
  count: number;
  tracks: FaceTrackInfo[];
}

const faceDetection = {
  getConfigReq: () => request.get('/api/v1/face/config'),
  setConfigReq: (data: Partial<FaceDetectionConfig>) => request.post('/api/v1/face/config', data),
  getStatusReq: () => request.get('/api/v1/face/status'),
  getResultsReq: () => request.get('/api/v1/face/results'),
  triggerReportReq: (trackId = 0) => request.post('/api/v1/face/trigger', { track_id: trackId }),
  // trigger response now carries { armed, report_enabled }
  resetReq: () => request.post('/api/v1/face/reset'),
  verifyReq: (file: File) => {
    const form = new FormData();
    form.append('image', file);
    return request.post('/api/v1/face/verify', form, {
      headers: { 'Content-Type': 'multipart/form-data' },
    });
  },
};

export default faceDetection;
