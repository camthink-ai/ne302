import request from '../request';

/** Counting line coordinates and all tuning fields are permille/percent-free
 * integers exactly as the firmware stores them (see people_counting_config.h). */
export interface PeopleCountingConfig {
  enabled: boolean;
  show_line: boolean;
  show_osd: boolean;
  face_mosaic: boolean;
  line_x1: number;
  line_y1: number;
  line_x2: number;
  line_y2: number;
  line_thickness: number;
  invert_direction: boolean;
  track_iou_threshold: number;
  track_max_missed: number;
  cross_debounce: number;
  min_box_area: number;
  mosaic_block: number;
  mosaic_expand: number;
  face_conf_threshold: number;
  face_nms_threshold: number;
  coord_max?: number;
}

export interface PeopleCountingStats {
  enabled: boolean;
  current_count: number;
  in_count: number;
  out_count: number;
  net_count: number;
  active_tracks: number;
  last_person_detections: number;
  last_face_detections: number;
  face_model_loaded: boolean;
  last_update_ms: number;
  face_model_name?: string;
}

const peopleCounting = {
  getCountingConfigReq: () => request.get('/api/v1/counting/config'),
  setCountingConfigReq: (data: Partial<PeopleCountingConfig>) => request.post('/api/v1/counting/config', data),
  getCountingStatsReq: () => request.get('/api/v1/counting/stats'),
  resetCountingReq: () => request.post('/api/v1/counting/reset'),
};

export default peopleCounting;
