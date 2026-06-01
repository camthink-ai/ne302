import { useState, useEffect } from 'react';
import { useLingui } from '@lingui/react';
import SvgIcon from '@/components/svg-icon';
import { Label } from '@/components/ui/label';
import { Separator } from '@/components/ui/separator';
import { Button } from '@/components/ui/button';
import CommunicationSkeleton from './skeleton';
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogClose, DialogFooter } from '@/components/dialog';
import {
    Popover,
    PopoverContent,
    PopoverTrigger,
} from '@/components/ui/popover';
import { Select, SelectTrigger, SelectValue, SelectContent, SelectItem } from '@/components/ui/select';
import { useIsMobile } from '@/hooks/use-mobile';
import { Input } from '@/components/ui/input';
import WifiReloadMask from '@/components/wifi-reload-mask';
import { sleep } from '@/utils';

type HalowData = {
    ssid: string;
    bssid: string;
    rssi: number;
    channel: number;
    security: string;
    connected: boolean;
    is_known: boolean;
    last_connected_time: number;
};

type HalowRegion = {
    value: string;
    labelKey: string;
};

const HALOW_REGIONS: HalowRegion[] = [
    { value: 'us', labelKey: 'sys.system_management.halow_region_us' },
    { value: 'eu', labelKey: 'sys.system_management.halow_region_eu' },
    { value: 'cn', labelKey: 'sys.system_management.halow_region_cn' },
    { value: 'jp', labelKey: 'sys.system_management.halow_region_jp' },
    { value: 'au', labelKey: 'sys.system_management.halow_region_au' },
];

const createMockHalowData = (region: string): {
    current: HalowData;
    known: HalowData[];
    unknown: HalowData[];
} => {
    const regionUpper = region.toUpperCase();
    return {
        current: {
            ssid: `HaLow-Gateway-${regionUpper}`,
            bssid: `00:11:22:33:44:${region === 'us' ? '01' : region === 'eu' ? '02' : '03'}`,
            rssi: region === 'cn' ? -48 : -62,
            channel: region === 'us' ? 12 : 8,
            security: 'wpa2',
            connected: true,
            is_known: true,
            last_connected_time: Date.now(),
        },
        known: [
            {
                ssid: `HaLow-Office-${regionUpper}`,
                bssid: '00:11:22:33:44:10',
                rssi: -58,
                channel: 6,
                security: 'wpa2',
                connected: false,
                is_known: true,
                last_connected_time: Date.now() - 86400000,
            },
            {
                ssid: `HaLow-Warehouse-${regionUpper}`,
                bssid: '00:11:22:33:44:11',
                rssi: -71,
                channel: 4,
                security: 'wpa2',
                connected: false,
                is_known: true,
                last_connected_time: Date.now() - 172800000,
            },
        ],
        unknown: [
            {
                ssid: `HaLow-Sensor-A-${regionUpper}`,
                bssid: '00:11:22:33:44:20',
                rssi: -65,
                channel: 10,
                security: 'wpa2',
                connected: false,
                is_known: false,
                last_connected_time: 0,
            },
            {
                ssid: `HaLow-Sensor-B-${regionUpper}`,
                bssid: '00:11:22:33:44:21',
                rssi: -78,
                channel: 2,
                security: 'open',
                connected: false,
                is_known: false,
                last_connected_time: 0,
            },
            {
                ssid: `HaLow-Field-${regionUpper}`,
                bssid: '00:11:22:33:44:22',
                rssi: -82,
                channel: 14,
                security: 'wpa2',
                connected: false,
                is_known: false,
                last_connected_time: 0,
            },
        ],
    };
};

export default function HalowNetworkPage() {
    const { i18n } = useLingui();
    const isMobile = useIsMobile();
    const [isLoading, setIsLoading] = useState(true);
    const [region, setRegion] = useState('cn');
    const [currentHalowData, setCurrentHalowData] = useState<HalowData | null>(null);
    const [knownHalowDataList, setKnownHalowDataList] = useState<HalowData[]>([]);
    const [otherHalowDataList, setOtherHalowDataList] = useState<HalowData[]>([]);
    const [isConnectDialogOpen, setIsConnectDialogOpen] = useState(false);
    const [isPasswordVisible, setIsPasswordVisible] = useState(false);
    const [halowPassword, setHalowPassword] = useState('');
    const [halowInfo, setHalowInfo] = useState<HalowData | null>(null);
    const [showReloadMask, setShowReloadMask] = useState(false);
    const [loadingText, setLoadingText] = useState('');
    const [isReloading, setIsReloading] = useState(false);
    const [isErrorPassword, setIsErrorPassword] = useState(false);

    const fetchMockHalowData = async (selectedRegion: string, showSkeleton = true) => {
        try {
            if (showSkeleton) setIsLoading(true);
            await sleep(500);
            const mock = createMockHalowData(selectedRegion);
            setCurrentHalowData(mock.current);
            setKnownHalowDataList(mock.known);
            setOtherHalowDataList(mock.unknown);
        } finally {
            if (showSkeleton) setIsLoading(false);
        }
    };

    useEffect(() => {
        fetchMockHalowData(region);
    }, [region]);

    const isValidatePassword = (password: string, minLength: number, maxLength: number) => {
        const allowedPattern = /^[a-zA-Z0-9!@#$%^&*()_+\-=[\]{}|;':",./<>?`~]+$/;
        if (!allowedPattern.test(password)) return false;
        if (password.length < minLength || password.length > maxLength) return false;
        return true;
    };

    const handleCancelConnect = () => {
        setIsConnectDialogOpen(false);
        setHalowPassword('');
        setIsErrorPassword(false);
        setHalowInfo(null);
    };

    const handlePasswordVisible = (e: MouseEvent) => {
        e.preventDefault();
        setIsPasswordVisible(!isPasswordVisible);
    };

    const openConnectDialog = (data: HalowData, type: 'known' | 'unknown') => {
        setHalowInfo(data);
        if (data.security !== 'open' && type === 'unknown') {
            setIsConnectDialogOpen(true);
            return;
        }
        handleConnect(data, type);
    };

    const handleConnect = async (data: HalowData, type: 'known' | 'unknown') => {
        if (!data) return;
        if (!isValidatePassword(halowPassword, 8, 64) && data.security !== 'open' && type === 'unknown') {
            setIsErrorPassword(true);
            return;
        }
        try {
            setLoadingText(i18n._('sys.system_management.connecting_network'));
            setIsReloading(true);
            setShowReloadMask(true);
            await sleep(1500);
            setCurrentHalowData({ ...data, connected: true });
            setKnownHalowDataList((prev) => prev.filter((item) => item.bssid !== data.bssid));
            setOtherHalowDataList((prev) => prev.filter((item) => item.bssid !== data.bssid));
        } finally {
            setIsConnectDialogOpen(false);
            setHalowPassword('');
            setShowReloadMask(false);
            setIsReloading(false);
        }
    };

    const handleDisconnect = async () => {
        setCurrentHalowData(null);
        await fetchMockHalowData(region, false);
    };

    const handleForget = (data: HalowData) => {
        setKnownHalowDataList((prev) => prev.filter((item) => item.bssid !== data.bssid));
        if (currentHalowData?.bssid === data.bssid) {
            setCurrentHalowData(null);
        }
    };

    const handleScan = async () => {
        setLoadingText(i18n._('sys.system_management.scanning_network'));
        setIsReloading(true);
        setShowReloadMask(true);
        await sleep(1500);
        await fetchMockHalowData(region, false);
        setShowReloadMask(false);
        setIsReloading(false);
    };

    const handleRegionChange = (value: string) => {
        setRegion(value);
    };

    useEffect(() => {
        if (!isValidatePassword(halowPassword, 8, 64) && isErrorPassword) {
            setIsErrorPassword(true);
        } else {
            setIsErrorPassword(false);
        }
    }, [halowPassword, isErrorPassword]);

    const connectDialog = () => (
        <Dialog
          open={isConnectDialogOpen}
          onOpenChange={(open) => {
                setIsConnectDialogOpen(open);
                if (!open) setHalowInfo(null);
            }}
        >

            <DialogContent className="mx-8" showCloseButton={false}>
                <DialogClose asChild onClick={() => handleCancelConnect()}>
                    <Button
                      variant="ghost"
                      onClick={() => handleCancelConnect()}
                      className="absolute right-4 top-4 h-8 w-8 p-0 rounded-full opacity-70 hover:opacity-100 transition-opacity z-10"
                    >
                        <SvgIcon icon="close" className="w-4 h-4" />
                    </Button>
                </DialogClose>
                <DialogHeader>
                    <DialogTitle>{i18n._('sys.system_management.halow_dialog_title')}</DialogTitle>
                </DialogHeader>
                <div className="my-4">
                    <div className="mb-2">
                        <p>{i18n._('sys.system_management.join_network')} {halowInfo?.ssid ?? ''}</p>
                    </div>
                    <div className="relative">
                        <Input
                          placeholder={i18n._('sys.system_management.wifi-dialog-placeholder')}
                          type={isPasswordVisible ? 'text' : 'password'}
                          value={halowPassword}
                          onChange={(e) => setHalowPassword((e.target as HTMLInputElement).value)}
                        />
                        <button
                          type="button"
                          onClick={handlePasswordVisible}
                          className="absolute top-1/2 -translate-y-1/2 right-0 flex items-center bg-transparent pr-4 border-none cursor-pointer disabled:opacity-50"
                        >
                            {isPasswordVisible ? (
                                <SvgIcon className="w-4 h-4" icon="visibility" />
                            ) : (
                                <SvgIcon className="w-4 h-4" icon="visibility_off" />
                            )}
                        </button>
                    </div>
                    {isErrorPassword && (
                        <p className="text-sm text-red-500 mt-1">{i18n._('sys.system_management.password_error')}</p>
                    )}
                </div>
                <DialogFooter>
                    <Button size="sm" className="w-1/2 md:w-auto" variant="outline" onClick={() => handleCancelConnect()}>{i18n._('common.cancel')}</Button>
                    <Button size="sm" className="w-1/2 md:w-auto" variant="primary" onClick={() => handleConnect(halowInfo as HalowData, 'unknown')}>{i18n._('common.confirm')}</Button>
                </DialogFooter>
            </DialogContent>
        </Dialog>
    );

    return (
        <div className="mt-2">
            <div className="flex gap-2 justify-between items-center mb-4">
                <Label className="text-sm font-bold text-text-primary">{i18n._('sys.system_management.halow_region')}</Label>
                <Select value={region} onValueChange={handleRegionChange}>
                    <SelectTrigger className="w-[180px]">
                        <SelectValue placeholder={i18n._('sys.system_management.halow_region')} />
                    </SelectTrigger>
                    <SelectContent>
                        {HALOW_REGIONS.map((item) => (
                            <SelectItem key={item.value} value={item.value}>
                                {i18n._(item.labelKey)}
                            </SelectItem>
                        ))}
                    </SelectContent>
                </Select>
            </div>

            {isLoading && <CommunicationSkeleton />}
            {!isLoading && (
                <div className="relative">
                    <Button
                      variant="ghost"
                      className="absolute -top-2 right-0"
                      onClick={handleScan}
                      title={i18n._('sys.system_management.refresh')}
                    >
                        <SvgIcon icon="reload2" className="w-6 h-6" />
                    </Button>

                    {!currentHalowData?.connected && knownHalowDataList.length === 0 && otherHalowDataList.length === 0 && (
                        <div className="h-[400px] flex flex-col items-center justify-center">
                            <SvgIcon icon="empty" className="w-40 h-40" />
                            <p className="text-sm text-text-secondary">{i18n._('sys.system_management.no_network')}</p>
                        </div>
                    )}

                    {currentHalowData?.connected && (
                        <>
                            <p className="text-sm font-bold mb-2">{i18n._('sys.system_management.communication_mode')}</p>
                            <div className="flex flex-col gap-2 bg-gray-100 p-4 rounded-lg">
                                <div className="flex justify-between">
                                    <div className="flex items-center gap-2">
                                        <div className="w-6 h-6 bg-primary rounded-md flex items-center justify-center">
                                            <SvgIcon icon="wifi-halow" className="w-4 h-4 text-white" />
                                        </div>
                                        <p>{i18n._('sys.system_management.halow')}</p>
                                    </div>
                                </div>
                                <Separator />
                                <div className="flex justify-between">
                                    <Label>{currentHalowData.ssid}</Label>
                                    <div className="flex items-center gap-1">
                                        <div className="flex items-center space-x-1 mr-1">
                                            <SvgIcon icon="check" className="w-4 h-4" />
                                            <p className="text-sm text-green-500">{i18n._('common.connected')}</p>
                                        </div>
                                        <SvgIcon icon={currentHalowData.rssi >= -55 ? 'wifi' : currentHalowData.rssi >= -75 ? 'wifi_middle' : 'wifi_low'} className="w-4 h-4 text-[#272E3B]" />
                                        <Popover>
                                            <PopoverTrigger onClick={(e: any) => e.stopPropagation()}>
                                                <SvgIcon icon="more" className="w-4 h-4 text-white cursor-pointer" />
                                            </PopoverTrigger>
                                            <PopoverContent className="w-auto p-0">
                                                <div className="flex flex-col m-1">
                                                    <div className="text-sm px-4 py-1 cursor-pointer hover:bg-gray-100 hover:rounded-md" onClick={() => handleDisconnect()}>{i18n._('sys.system_management.disconnect')}</div>
                                                    <Separator className="my-1" />
                                                    <div className="text-sm px-4 py-1 cursor-pointer hover:bg-gray-100 hover:rounded-md" onClick={() => handleForget(currentHalowData)}>{i18n._('sys.system_management.forget')}</div>
                                                </div>
                                            </PopoverContent>
                                        </Popover>
                                    </div>
                                </div>
                            </div>
                        </>
                    )}

                    {knownHalowDataList.length > 0 && (
                        <div className="mt-4">
                            <p className="text-sm font-bold mb-2">{i18n._('sys.system_management.known_network')}</p>
                            <div className="flex flex-col bg-gray-100 px-4 rounded-lg">
                                {knownHalowDataList.map((item, index) => (
                                    <div onClick={() => { if (isMobile) openConnectDialog(item, 'known'); }} key={`${item.ssid}-${item.bssid}-${index}`} className="group">
                                        <div className="flex justify-between py-2">
                                            <Label>{item.ssid}</Label>
                                            <div className="flex items-center gap-1">
                                                <Button size="sm" variant="outline" onClick={() => openConnectDialog(item, 'known')} className="mr-2 opacity-0 group-hover:opacity-100 transition-opacity">{i18n._('common.connect')}</Button>
                                                <SvgIcon icon={item.rssi >= -55 ? 'wifi' : item.rssi >= -75 ? 'wifi_middle' : 'wifi_low'} className="w-4 h-4 text-[#272E3B]" />
                                                <Popover>
                                                    <PopoverTrigger onClick={(e: any) => e.stopPropagation()}>
                                                        <SvgIcon icon="more" className="w-4 h-4 text-white cursor-pointer" />
                                                    </PopoverTrigger>
                                                    <PopoverContent className="w-auto p-0">
                                                        <div className="flex flex-col m-1">
                                                            <div className="text-sm px-4 py-1 cursor-pointer hover:bg-gray-100 hover:rounded-md" onClick={() => handleForget(item)}>{i18n._('sys.system_management.forget')}</div>
                                                        </div>
                                                    </PopoverContent>
                                                </Popover>
                                            </div>
                                        </div>
                                        {index !== knownHalowDataList.length - 1 && <Separator />}
                                    </div>
                                ))}
                            </div>
                        </div>
                    )}

                    {otherHalowDataList.length > 0 && (
                        <div className="mt-4">
                            <p className="text-sm font-bold mb-2">{i18n._('sys.system_management.other_network')}</p>
                            <div className="flex flex-col bg-gray-100 px-4 rounded-lg">
                                {otherHalowDataList.map((item, index) => (
                                    <div onClick={() => { if (isMobile) openConnectDialog(item, 'unknown'); }} key={`${item.ssid}-${item.bssid}-${index}`} className="group">
                                        <div className="flex justify-between py-2">
                                            <Label className="text-sm">{item.ssid}</Label>
                                            <div className="flex items-center gap-1">
                                                <Button size="sm" variant="outline" onClick={() => openConnectDialog(item, 'unknown')} className="mr-2 opacity-0 group-hover:opacity-100 transition-opacity">{i18n._('common.connect')}</Button>
                                                {item.security !== 'open' && <SvgIcon icon="lock" className="w-4 h-4 mr-1" />}
                                                <SvgIcon icon={item.rssi >= -55 ? 'wifi' : item.rssi >= -75 ? 'wifi_middle' : 'wifi_low'} className="w-4 h-4 text-[#272E3B]" />
                                            </div>
                                        </div>
                                        {index !== otherHalowDataList.length - 1 && <Separator />}
                                    </div>
                                ))}
                            </div>
                        </div>
                    )}
                </div>
            )}
            {connectDialog()}
            {showReloadMask && <WifiReloadMask loadingText={loadingText} isLoading={isReloading} />}
        </div>
    );
}
