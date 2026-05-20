/*
 * @Description: Browser detection utility
 */

class BrowserDetector {
    private userAgent: string

    constructor() {
        this.userAgent = navigator.userAgent.toLowerCase()
    }

    // Detect if browser is Chrome
    isBrowserChrome() {
        return this.userAgent.includes('chrome') && !this.userAgent.includes('edg')
    }

    // Detect if browser is Safari
    isBrowserSafari() {
        return this.userAgent.includes('safari') && !this.userAgent.includes('chrome')
    }

    // Detect if browser is Firefox
    isBrowserFirefox() {
        return this.userAgent.includes('firefox')
    }

    // Detect if browser is Edge
    isBrowserEdge() {
        return this.userAgent.includes('edg')
    }

    // Detect if browser is IE
    isBrowserIE() {
        return this.userAgent.includes('msie') || this.userAgent.includes('trident')
    }

    // Get browser name
    getBrowserName() {
        if (this.isBrowserChrome()) return 'Chrome'
        if (this.isBrowserSafari()) return 'Safari'
        if (this.isBrowserFirefox()) return 'Firefox'
        if (this.isBrowserEdge()) return 'Edge'
        if (this.isBrowserIE()) return 'IE'
        return 'Unknown'
    }

    // Get browser version
    getBrowserVersion() {
        const match = this.userAgent.match(/(chrome|safari|firefox|edg|msie|trident(?=\/))\/?\s*(\d+)/i)
        return match ? match[2] : 'Unknown'
    }

    // Detect if WebGL is supported
    static isWebGLSupported(): boolean {
        try {
            const canvas = document.createElement('canvas')
            // eslint-disable-next-line @typescript-eslint/no-unused-vars
            return !!(canvas.getContext('webgl') || canvas.getContext('experimental-webgl'))
        } catch (e) {
            console.error(e)
            return false
        }
    }

    // Detect if MSE is supported
    static isMSESupported(): boolean {
        // eslint-disable-next-line @typescript-eslint/no-unused-vars
        return 'MediaSource' in window
    }

    // Detect if H.264 is supported
    static isH264Supported(): boolean {
        const video = document.createElement('video')
        return video.canPlayType('video/mp4; codecs="avc1.42E01E"') !== ''
    }

    // Detect if AAC is supported
    static isAACSupported(): boolean {
        const video = document.createElement('video')
        return video.canPlayType('audio/mp4; codecs="mp4a.40.2"') !== ''
    }
}

export default new BrowserDetector() 