import request from '../request'

export interface SetHardwareInfoReq {
    brightness: number;
    contrast: number;
    horizontal_flip: boolean;
    vertical_flip: boolean;
    aec: number;
    fast_capture_skip_frames: number;
    fast_capture_resolution: number;
    fast_capture_jpeg_quality: number;
    capture_disable_comm: boolean;
    capture_storage_ai: boolean;
}
export interface SetLightConfigReq {
    mode: 'auto' | 'custom' | 'off';
    brightness_level: number;
    connected?: boolean;
    custom_schedule: {
        start_hour: number;
        start_minute: number;
        end_hour: number;
        end_minute: number;
    }
}

const hardwareManagement = {
    getHardwareInfoReq: () => request.get('/api/v1/device/image/config'),
    setHardwareInfoReq: (data: SetHardwareInfoReq) => request.post('/api/v1/device/image/config', data),
    getLightConfigReq: () => request.get('/api/v1/device/light/config'),
    setLightConfigReq: (data: SetLightConfigReq) => request.post('/api/v1/device/light/config', data),
}

export default hardwareManagement;