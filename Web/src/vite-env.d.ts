/// <reference types="vite/client" />

declare const __APP_VERSION__: string;

import 'axios';

declare module 'axios' {
  interface AxiosRequestConfig {
    skipErrorToast?: boolean;
  }
}
