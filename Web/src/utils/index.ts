const sleep = (ms: number): Promise<void> => new Promise(resolve => {
  setTimeout(resolve, ms);
});

// file to base64
const fileToBase64 = (file: File): Promise<string> => new Promise((resolve, reject) => {
  const reader = new FileReader();
  reader.onload = () => resolve(reader.result as string);
  reader.onerror = () => reject(new Error('Failed to read file'));
  reader.readAsDataURL(file);
});

/**
 * Async request function with retry mechanism
 * @param promiseFn - Function that returns Promise; receives AbortSignal to cancel on timeout
 * @param timeout - Timeout for each request (milliseconds)
 * @param retryCount - Maximum retry count
 */
const retryFetch = async (
  promiseFn: (signal?: AbortSignal) => Promise<any>,
  timeout: number = 3000,
  retryCount: number = 3
): Promise<any> => {
  /* eslint-disable no-await-in-loop */
  for (let i = 0; i < retryCount; i++) {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), timeout);
    try {
      const result = await promiseFn(controller.signal);
      return result;
    } catch (error) {
      if (i === retryCount - 1) throw error;
    } finally {
      clearTimeout(timeoutId);
    }
  }
  /* eslint-enable no-await-in-loop */
};

// Concurrent promise
/**
 * Concurrently execute Promise array with concurrency control
 * @param tasks Task array, can be Promise or function returning Promise
 * @param concurrency Maximum concurrency, default 3
 * @param options Configuration options
 * @returns Promise<ConcurrentResult<T>>
 */
interface ConcurrentOptions {
  onProgress?: (completed: number, total: number) => void;
  onTaskComplete?: (index: number, result: any) => void;
  onTaskError?: (index: number, error: any) => void;
  stopOnError?: boolean; // Whether to stop on error, default false
}

interface ConcurrentResult<T> {
  results: (T | Error)[];
  completed: number;
  failed: number;
  errors: Error[];
}

const concurrentPromise = async <T = any>(
  tasks: (() => Promise<T> | Promise<T>)[],
  concurrency: number = 3,
  options: ConcurrentOptions = {}
): Promise<ConcurrentResult<T>> => {
  const {
    onProgress,
    onTaskComplete,
    onTaskError,
    stopOnError = false,
  } = options;

  const results: (T | Error)[] = new Array(tasks.length);
  const errors: Error[] = [];
  let completed = 0;
  let failed = 0;

  // Execute single task
  const executeTask = async (index: number): Promise<void> => {
    try {
      const task = tasks[index];
      const promise = typeof task === 'function' ? task() : task;
      const result = await promise;

      results[index] = result;
      completed++;
      onTaskComplete?.(index, result);
    } catch (error) {
      const err = error instanceof Error ? error : new Error(String(error));
      results[index] = err;
      errors.push(err);
      failed++;
      completed++;
      onTaskError?.(index, error);

      if (stopOnError) {
        throw err;
      }
    }

    onProgress?.(completed, tasks.length);
  };

  // Execute tasks in batches
  const executeBatches = async (): Promise<void> => {
    const taskPromises = tasks.map((_, index) => executeTask(index));

    // Process in batches to avoid using await in loop
    const batches: Promise<void>[][] = [];
    for (let i = 0; i < taskPromises.length; i += concurrency) {
      batches.push(taskPromises.slice(i, i + concurrency));
    }

    // Execute all batches
    const batchPromises = batches.map(batch => Promise.all(batch));
    await Promise.all(batchPromises);
  };

  try {
    await executeBatches();
  } catch (error) {
    // If stopOnError is set, catch the first error here
    if (stopOnError) {
      throw error;
    }
  }

  return {
    results,
    completed,
    failed,
    errors,
  };
};

/**
 * Batch process data with concurrency control
 * @param data Data array to process
 * @param processor Processing function
 * @param concurrency Concurrency number
 * @param options Configuration options
 */
const batchProcess = async <T, R>(
  data: T[],
  processor: (item: T, index: number) => Promise<R>,
  concurrency: number = 3,
  options: Omit<ConcurrentOptions, 'onTaskComplete' | 'onTaskError'> & {
    onItemComplete?: (item: T, result: R, index: number) => void;
    onItemError?: (item: T, error: any, index: number) => void;
  } = {}
): Promise<ConcurrentResult<R>> => {
  const tasks = data.map((item, index) => () => processor(item, index));

  return concurrentPromise(tasks, concurrency, {
    ...options,
    onTaskComplete: (index, result) => {
      options.onItemComplete?.(data[index], result, index);
    },
    onTaskError: (index, error) => {
      options.onItemError?.(data[index], error, index);
    },
  });
};

/**
 * @TODO 
 * - Get current timezone and timestamp
 * - Convert corresponding timestamp to UTC timestamp
 * - Return timestamp and timezone {timestamp,timezone}
 * 
 */
interface TimeData {
  timestamp: number;
  timezone: string;
}

const getCurrentTimestampTOUTC = (): TimeData => {
  // 1. Get current local timestamp (based on current moment, unit: seconds)
  const ts = Math.floor(Date.now() / 1000);

  // 2. Get difference between local time and UTC (unit: minutes, UTC - Local)
  // getTimezoneOffset() returns minutes, for UTC+8, value is -480
  const offsetMinutes = new Date().getTimezoneOffset();
  const offsetSeconds = offsetMinutes * 60;

  // 3. Convert to UTC timestamp: local timestamp - offset seconds
  // Note: getTimezoneOffset() returns UTC - Local, so use subtraction
  const utcTimestamp = ts - offsetSeconds;

  // 4. Return required structure
  return {
    timestamp: utcTimestamp,
    timezone: 'UTC'
  };
}

const downloadFile = async (data: string | Blob | ArrayBuffer, filename: string) => {
  let blob: Blob;

  if (typeof data === 'string') {
    if (data.startsWith('http')) {
      const res = await fetch(data);
      blob = await res.blob();
    } else {
      blob = new Blob([data], { type: 'text/plain;charset=utf-8' });
    }
  } else if (data instanceof Blob) {
    blob = data;
  } else {
    blob = new Blob([data]);
  }

  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  a.remove();
  URL.revokeObjectURL(url);
};
/**
 * Get IP address or hostname from current webpage URL
 * @returns Returns IP address or hostname, e.g., "192.168.10.10" or "localhost"
 */
const getHostFromUrl = (): string => {
  if (import.meta.env.MODE === 'development') {
    const wsUrl = import.meta.env.VITE_Websocket_URL;
    if (wsUrl) {
      return wsUrl;
    }
    return '192.168.10.10';
  }
  return window.location.hostname;
}

/**
 * Get complete WebSocket URL
 * @param path WebSocket path, default is '/stream'
 * @param port Port number, default is 8081
 * @returns Complete WebSocket URL, e.g., "ws://192.168.10.10:8081/stream"
 */
const getWebSocketUrl = (path: string = '/stream', port: number = 8081): string => {
  const host = getHostFromUrl();
  return `ws://${host}:${port}${path}`;
}

/**
 * Slice binary file head content and return a new File (default first 1KB)
 * @param file File to slice
 * @param bytes Byte length to read
 */
const sliceFile = (file: File, bytes = 1024): Promise<File> => Promise.resolve(new File(
  [file.slice(0, bytes)],
  file.name,
  {
    type: file.type || 'application/octet-stream',
    lastModified: file.lastModified,
  },
));

export { sleep, fileToBase64, retryFetch, concurrentPromise, batchProcess, getCurrentTimestampTOUTC, downloadFile, getHostFromUrl, getWebSocketUrl, sliceFile };
