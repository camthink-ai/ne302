import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { renderCanvasImg, convertToJpeg, fixedScaleImg, proportionScaleImg } from '@/utils/renderCanvasImg';

// Mock canvas and context
const mockCanvas = {
  width: 0,
  height: 0,
  getContext: vi.fn(),
  toDataURL: vi.fn(),
} as unknown as HTMLCanvasElement;

const mockContext = {
  drawImage: vi.fn(),
  strokeStyle: '',
  lineWidth: 0,
  strokeRect: vi.fn(),
  font: '',
  measureText: vi.fn(),
  fillStyle: '',
  fillRect: vi.fn(),
  fillText: vi.fn(),
} as unknown as CanvasRenderingContext2D;

// Mock Image constructor
const mockImage = vi.fn();
const mockImageInstance = {
  crossOrigin: '',
  naturalWidth: 800,
  naturalHeight: 600,
  width: 800,
  height: 600,
  onload: null as (() => void) | null,
  onerror: null as (() => void) | null,
  src: '',
};

describe('renderCanvasImg', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    
    // Mock document.createElement
    vi.spyOn(document, 'createElement').mockReturnValue(mockCanvas);
    
    // Mock canvas.getContext
    // @ts-expect-error mock canvas getContext in unit tests
    mockCanvas.getContext.mockReturnValue(mockContext);
    
    // Mock Image constructor
    global.Image = mockImage as any;
    mockImage.mockReturnValue(mockImageInstance);
    
    // Mock measureText
    // @ts-expect-error mock canvas measureText in unit tests
    mockContext.measureText.mockReturnValue({ width: 100 });
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('should create canvas and render image with detections', async () => {
    const imageSrc = 'test-image.jpg';
    const detections = [
      {
        class: 'person',
        confidence: 0.95,
        bbox: { x: 100, y: 100, width: 200, height: 300 }
      },
      {
        class: 'car',
        confidence: 0.87,
        bbox: { x: 400, y: 200, width: 150, height: 100 }
      }
    ];

    const options = {
      strokeColor: '#FF0000',
      lineWidth: 3,
      fontSize: 16,
      showConfidence: true,
      showClass: true
    };

    // Start the promise
    const canvasPromise = renderCanvasImg(imageSrc, detections, options);

    // Simulate image load success
    mockImageInstance.onload?.();
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    // Wait for the promise to resolve\
    // eslint-disable-next-line @typescript-eslint/no-unused-vars
    await canvasPromise;

    // Verify canvas creation
    expect(document.createElement).toHaveBeenCalledWith('canvas');
    expect(mockCanvas.getContext).toHaveBeenCalledWith('2d');

    // Verify image setup
    expect(mockImageInstance.crossOrigin).toBe('anonymous');
    expect(mockImageInstance.src).toBe(imageSrc);

    // Verify canvas dimensions
    expect(mockCanvas.width).toBe(800);
    expect(mockCanvas.height).toBe(600);

    // Verify drawing operations
    expect(mockContext.drawImage).toHaveBeenCalledWith(mockImageInstance, 0, 0);
    expect(mockContext.strokeRect).toHaveBeenCalledTimes(2);
    expect(mockContext.fillRect).toHaveBeenCalledTimes(2);
    expect(mockContext.fillText).toHaveBeenCalledTimes(2);

    // Verify specific detection rendering
    expect(mockContext.strokeRect).toHaveBeenCalledWith(100, 100, 200, 300);
    expect(mockContext.strokeRect).toHaveBeenCalledWith(400, 200, 150, 100);

    // Verify label rendering
    expect(mockContext.fillText).toHaveBeenCalledWith('person 95.0%', 104, 96);
    expect(mockContext.fillText).toHaveBeenCalledWith('car 87.0%', 404, 196);
  });

  it('should handle missing canvas context', async () => {
    // @ts-expect-error simulate missing canvas context in unit tests
    mockCanvas.getContext.mockReturnValue(null);

    const canvasPromise = renderCanvasImg('test.jpg', [], {});
    
    await expect(canvasPromise).rejects.toThrow('no ctx');
  });

  it('should handle image load error', async () => {
    const canvasPromise = renderCanvasImg('test.jpg', [], {});

    // Simulate image load error
    mockImageInstance.onerror?.();

    await expect(canvasPromise).rejects.toThrow('image load error');
  });

  it('should skip detections with invalid bbox', async () => {
    const detections = [
      {
        class: 'person',
        confidence: 0.95,
        bbox: { x: 0, y: 0, width: 0, height: 0 } // Invalid bbox
      },
      {
        class: 'car',
        confidence: 0.87,
        bbox: { x: 100, y: 100, width: 200, height: 300 } // Valid bbox
      }
    ];

    const canvasPromise = renderCanvasImg('test.jpg', detections, {});

    // Simulate image load success
    mockImageInstance.onload?.();

    await canvasPromise;

    // Should only render the valid detection
    expect(mockContext.strokeRect).toHaveBeenCalledTimes(1);
    expect(mockContext.strokeRect).toHaveBeenCalledWith(100, 100, 200, 300);
  });

  it('should use default options when none provided', async () => {
    const detections = [
      {
        class: 'person',
        confidence: 0.95,
        bbox: { x: 100, y: 100, width: 200, height: 300 }
      }
    ];

    const canvasPromise = renderCanvasImg('test.jpg', detections);

    // Simulate image load success
    mockImageInstance.onload?.();

    await canvasPromise;

    // Should use default values
    expect(mockContext.strokeStyle).toBe('#F00');
    expect(mockContext.lineWidth).toBe(2);
    expect(mockContext.font).toBe('14px Arial');
  });

  it('should handle empty detections array', async () => {
    const canvasPromise = renderCanvasImg('test.jpg', []);

    // Simulate image load success
    mockImageInstance.onload?.();

    await canvasPromise;

    // Should not call any drawing methods
    expect(mockContext.strokeRect).not.toHaveBeenCalled();
    expect(mockContext.fillRect).not.toHaveBeenCalled();
    expect(mockContext.fillText).not.toHaveBeenCalled();
  });
});

// Mock File constructor
const createMockFile = (name: string, type: string, size: number = 1024) => {
  const file = new File(['mock content'], name, { type });
  Object.defineProperty(file, 'size', { value: size });
  return file;
};

// Mock Blob
const mockBlob = {
  size: 1024,
  type: 'image/jpeg'
} as Blob;

describe('convertToJpeg', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    
    // Mock document.createElement
    vi.spyOn(document, 'createElement').mockReturnValue(mockCanvas);
    
    // Mock canvas.getContext
    // @ts-expect-error mock canvas getContext in unit tests
    mockCanvas.getContext.mockReturnValue(mockContext);
    
    // Mock Image constructor
    global.Image = mockImage as any;
    mockImage.mockReturnValue(mockImageInstance);
    
    // Mock URL.createObjectURL and revokeObjectURL
    global.URL.createObjectURL = vi.fn().mockReturnValue('mock-url');
    global.URL.revokeObjectURL = vi.fn();
    
    // Mock canvas.toBlob
    mockCanvas.toBlob = vi.fn().mockImplementation((callback) => {
      callback(mockBlob);
    });
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('should convert file to JPEG blob', async () => {
    const file = createMockFile('test.png', 'image/png');
    
    const promise = convertToJpeg(file);
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    const result = await promise;
    
    expect(result).toBe(mockBlob);
    expect(mockCanvas.toBlob).toHaveBeenCalledWith(
      expect.any(Function),
      'image/jpeg',
      1
    );
  });

  it('should handle image load error', async () => {
    const file = createMockFile('test.png', 'image/png');
    
    const promise = convertToJpeg(file);
    
    // Simulate image error
    mockImageInstance.onerror?.();
    
    await expect(promise).rejects.toThrow('Image load error');
  });

  it('should handle canvas context not available', async () => {
    // @ts-expect-error simulate missing canvas context in unit tests
    mockCanvas.getContext.mockReturnValue(null);
    
    const file = createMockFile('test.png', 'image/png');
    
    const promise = convertToJpeg(file);
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    await expect(promise).rejects.toThrow('Canvas not supported');
  });

  it('should handle blob creation failure', async () => {
    mockCanvas.toBlob = vi.fn().mockImplementation((callback) => {
      callback(null);
    });
    
    const file = createMockFile('test.png', 'image/png');
    
    const promise = convertToJpeg(file);
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    await expect(promise).rejects.toThrow('Failed to convert');
  });
});

describe('proportionScaleImg', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    
    // Mock document.createElement
    vi.spyOn(document, 'createElement').mockReturnValue(mockCanvas);
    
    // Mock canvas.getContext
    // @ts-expect-error mock canvas getContext in unit tests
    mockCanvas.getContext.mockReturnValue(mockContext);
    
    // Mock Image constructor
    global.Image = mockImage as any;
    mockImage.mockReturnValue(mockImageInstance);
    
    // Mock URL methods
    global.URL.createObjectURL = vi.fn().mockReturnValue('mock-url');
    global.URL.revokeObjectURL = vi.fn();
    
    // Mock canvas.toBlob
    mockCanvas.toBlob = vi.fn().mockImplementation((callback) => {
      callback(mockBlob);
    });
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('should scale down large image proportionally', async () => {
    // Mock large image
    mockImageInstance.width = 1440;
    mockImageInstance.height = 900;
    mockImageInstance.naturalWidth = 1440;
    mockImageInstance.naturalHeight = 900;
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = proportionScaleImg({ file, maxSize: 720 });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    const result = await promise;
    
    expect(result.width).toBe(720);
    expect(result.height).toBe(448);
    expect(result.scaledFile).toBeInstanceOf(File);
    expect(mockContext.drawImage).toHaveBeenCalledWith(mockImageInstance, 0, 0, 720, 448);
  }); 

  it('should handle vertical images', async () => {
    // Mock vertical image
    mockImageInstance.width = 600;
    mockImageInstance.height = 1200;
    mockImageInstance.naturalWidth = 600;
    mockImageInstance.naturalHeight = 1200;
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = proportionScaleImg({ file, maxSize: 720 });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    const result = await promise;
    
    expect(result.width).toBe(352); 
    expect(result.height).toBe(720);
  });

  it('should return original file if no scaling needed', async () => {
    // Mock small image
    mockImageInstance.width = 500;
    mockImageInstance.height = 400;
    mockImageInstance.naturalWidth = 500;
    mockImageInstance.naturalHeight = 400;
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = proportionScaleImg({ file, maxSize: 720 });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    const result = await promise;
    
    expect(result.width).toBe(496);
    expect(result.height).toBe(384);
    expect(result.scaledFile).toBeInstanceOf(File);
    expect(mockContext.drawImage).toHaveBeenCalledWith(mockImageInstance, 0, 0, 496, 384);
  });

  it('should handle non-image files', async () => {
    const file = createMockFile('test.txt', 'text/plain');
    
    const promise = proportionScaleImg({ file });
    
    await expect(promise).rejects.toThrow('File is not an image');
  });

  it('should handle canvas context not available', async () => {
    // @ts-expect-error simulate missing canvas context in unit tests
    mockCanvas.getContext.mockReturnValue(null);
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = proportionScaleImg({ file });
    
    await expect(promise).rejects.toThrow('Failed to get canvas context');
  });

  it('should handle image load error', async () => {
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = proportionScaleImg({ file });
    
    // Simulate image error
    mockImageInstance.onerror?.();
    
    await expect(promise).rejects.toThrow('Failed to load image');
    expect(global.URL.revokeObjectURL).toHaveBeenCalled();
  });

  it('should handle custom quality', async () => {
    mockImageInstance.width = 1440;
    mockImageInstance.height = 900;
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = proportionScaleImg({ file, maxSize: 720, quality: 0.8 });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    await promise;
    
    expect(mockCanvas.toBlob).toHaveBeenCalledWith(
      expect.any(Function),
      'image/jpeg',
      0.8
    );
  });
});

describe('fixedScaleImg', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    
    // Mock document.createElement
    vi.spyOn(document, 'createElement').mockReturnValue(mockCanvas);
    
    // Mock canvas.getContext
    // @ts-expect-error mock canvas getContext in unit tests
    mockCanvas.getContext.mockReturnValue(mockContext);
    
    // Mock Image constructor
    global.Image = mockImage as any;
    mockImage.mockReturnValue(mockImageInstance);
    
    // Mock URL methods
    global.URL.createObjectURL = vi.fn().mockReturnValue('mock-url');
    global.URL.revokeObjectURL = vi.fn();
    
    // Mock canvas.toBlob
    mockCanvas.toBlob = vi.fn().mockImplementation((callback) => {
      callback(mockBlob);
    });
  });

  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('should scale image to fixed dimensions', async () => {
    mockImageInstance.width = 800;
    mockImageInstance.height = 600;
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = fixedScaleImg({ 
      file, 
      aiImgWidth: 480, 
      aiImgHeight: 480 
    });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    const result = await promise;
    
    expect(result.width).toBe(480);
    expect(result.height).toBe(480);
    expect(result.scaledFile).toBeInstanceOf(File);
    expect(mockCanvas.width).toBe(480);
    expect(mockCanvas.height).toBe(480);
    expect(mockContext.drawImage).toHaveBeenCalledWith(mockImageInstance, 0, 0, 480, 480);
  });

  it('should handle non-image files', async () => {
    const file = createMockFile('test.txt', 'text/plain');
    
    const promise = fixedScaleImg({ 
      file, 
      aiImgWidth: 480, 
      aiImgHeight: 480 
    });
    
    await expect(promise).rejects.toThrow('File is not an image');
  });

  it('should handle canvas context not available', async () => {
    // @ts-expect-error simulate missing canvas context in unit tests
    mockCanvas.getContext.mockReturnValue(null);
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = fixedScaleImg({ 
      file, 
      aiImgWidth: 480, 
      aiImgHeight: 480 
    });
    
    await expect(promise).rejects.toThrow('Failed to get canvas context');
  });

  it('should handle image load error', async () => {
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = fixedScaleImg({ 
      file, 
      aiImgWidth: 480, 
      aiImgHeight: 480 
    });
    
    // Simulate image error
    mockImageInstance.onerror?.();
    
    await expect(promise).rejects.toThrow('Failed to load image');
    expect(global.URL.revokeObjectURL).toHaveBeenCalled();
  });

  it('should handle blob creation failure', async () => {
    mockCanvas.toBlob = vi.fn().mockImplementation((callback) => {
      callback(null);
    });
    
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = fixedScaleImg({ 
      file, 
      aiImgWidth: 480, 
      aiImgHeight: 480 
    });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    await expect(promise).rejects.toThrow('Failed to create blob');
  });

  it('should handle custom quality', async () => {
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = fixedScaleImg({ 
      file, 
      aiImgWidth: 480, 
      aiImgHeight: 480,
      quality: 0.9 
    });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    await promise;
    
    expect(mockCanvas.toBlob).toHaveBeenCalledWith(
      expect.any(Function),
      'image/jpeg',
      0.9
    );
  });

  it('should handle different target dimensions', async () => {
    const file = createMockFile('test.jpg', 'image/jpeg');
    
    const promise = fixedScaleImg({ 
      file, 
      aiImgWidth: 640, 
      aiImgHeight: 360 
    });
    
    // Simulate image load
    mockImageInstance.onload?.();
    
    const result = await promise;
    
    expect(result.width).toBe(640);
    expect(result.height).toBe(360);
    expect(mockCanvas.width).toBe(640);
    expect(mockCanvas.height).toBe(360);
    expect(mockContext.drawImage).toHaveBeenCalledWith(mockImageInstance, 0, 0, 640, 360);
  });
});