import { useState, useEffect, useCallback } from 'react';
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
import { sleep, retryFetch } from '@/utils';
import systemSettings from '@/services/api/systemSettings';

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

const filterOtherNetworks = (list: HalowData[], connected: HalowData | null) => {
    if (!connected?.connected) return list;
    return list.filter(
        (item) => !(item.ssid === connected.ssid && (!connected.bssid || item.bssid === connected.bssid))
    );
};

export default function HalowNetworkPage() {
    const { i18n } = useLingui();
    const isMobile = useIsMobile();
    const {
        getHalowStaReq,
        setHalowRegionReq,
        scanHalow,
        setHalow,
        disconnectHalow,
        deleteHalow,
    } = systemSettings;

    const [isLoading, setIsLoading] = useState(true);
    const [region, setRegion] = useState('us');
    const [supportedRegions, setSupportedRegions] = useState<string[]>(['us', 'eu', 'cn', 'jp', 'au']);
    const [currentHalowData, setCurrentHalowData] = useState<HalowData | null>(null);
    const [otherHalowDataList, setOtherHalowDataList] = useState<HalowData[]>([]);
    const [isConnectDialogOpen, setIsConnectDialogOpen] = useState(false);
    const [isPasswordVisible, setIsPasswordVisible] = useState(false);
    const [halowPassword, setHalowPassword] = useState('');
    const [halowInfo, setHalowInfo] = useState<HalowData | null>(null);
    const [showReloadMask, setShowReloadMask] = useState(false);
    const [loadingText, setLoadingText] = useState('');
    const [isReloading, setIsReloading] = useState(false);
    const [isErrorPassword, setIsErrorPassword] = useState(false);

    const getRegionLabel = useCallback((code: string) => {
        const key = `sys.system_management.halow_region_${code.toLowerCase()}`;
        const label = i18n._(key);
        if (label && label !== key) return label;
        return code.toUpperCase();
    }, [i18n]);

    const applyStaResponse = useCallback((data: any) => {
        if (data.region) {
            setRegion(String(data.region).toLowerCase());
        }
        if (Array.isArray(data.supported_regions) && data.supported_regions.length > 0) {
            setSupportedRegions(data.supported_regions.map((r: string) => String(r).toLowerCase()));
        }

        const connected: HalowData | null = data.connected
            ? {
                ssid: data.ssid ?? '',
                bssid: data.bssid ?? '',
                rssi: data.rssi ?? 0,
                channel: data.channel ?? 0,
                security: data.security ?? 'open',
                connected: true,
                is_known: true,
                last_connected_time: data.last_connected_time ?? 0,
            }
            : null;

        setCurrentHalowData(connected);

        const unknown: HalowData[] = data.scan_results?.unknown_networks ?? [];
        setOtherHalowDataList(filterOtherNetworks(unknown, connected));
    }, []);

    const getHalowSta = useCallback(async (showSkeleton = true) => {
        try {
            if (showSkeleton) setIsLoading(true);
            const res = await getHalowStaReq();
            applyStaResponse(res.data);
        } catch (error) {
            // eslint-disable-next-line no-console
            console.error(error);
        } finally {
            if (showSkeleton) setIsLoading(false);
        }
    }, [applyStaResponse, getHalowStaReq]);

    useEffect(() => {
        getHalowSta();
    }, [getHalowSta]);

    const reloadMask = async (
        fetchFn: () => Promise<any>,
        loadingTime: number,
        loadCount: number,
        maskText: string
    ) => {
        setLoadingText(maskText);
        setIsReloading(true);
        setShowReloadMask(true);
        try {
            await fetchFn();
            await retryFetch(() => getHalowSta(false), loadingTime, loadCount);
        } finally {
            setShowReloadMask(false);
            setIsReloading(false);
        }
    };

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

    const openConnectDialog = (data: HalowData) => {
        setHalowInfo(data);
        if (data.security !== 'open') {
            setIsConnectDialogOpen(true);
            return;
        }
        handleConnect(data);
    };

    const handleConnect = async (data: HalowData) => {
        if (!data) return;
        if (!isValidatePassword(halowPassword, 8, 64) && data.security !== 'open') {
            setIsErrorPassword(true);
            return;
        }
        const connectFn = () => setHalow({
            interface: 'halow',
            ssid: data.ssid,
            bssid: data.bssid,
            password: halowPassword,
            region,
        });
        try {
            await reloadMask(connectFn, 3000, 3, i18n._('sys.system_management.connecting_network'));
        } catch (error) {
            // eslint-disable-next-line no-console
            console.error(error);
        } finally {
            setIsConnectDialogOpen(false);
            setHalowPassword('');
            setHalowInfo(null);
        }
    };

    const handleDisconnect = async () => {
        try {
            await disconnectHalow({ interface: 'halow' });
            await handleScan();
        } catch (error) {
            // eslint-disable-next-line no-console
            console.error(error);
        }
    };

    const handleForget = async (data: HalowData) => {
        try {
            if (data.connected) {
                await disconnectHalow({ interface: 'halow' });
            }
            await deleteHalow({ ssid: data.ssid, bssid: data.bssid });
            await handleScan();
        } catch (error) {
            // eslint-disable-next-line no-console
            console.error(error);
        }
    };

    const handleScan = async () => {
        const waitScan = async () => {
            await scanHalow();
            await sleep(3000);
        };
        await reloadMask(waitScan, 3000, 3, i18n._('sys.system_management.scanning_network'));
    };

    const handleRegionChange = async (value: string) => {
        if (currentHalowData?.connected) return;
        try {
            setRegion(value);
            await setHalowRegionReq({ region: value });
            await handleScan();
        } catch (error) {
            // eslint-disable-next-line no-console
            console.error(error);
        }
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
                    <Button size="sm" className="w-1/2 md:w-auto" variant="primary" onClick={() => handleConnect(halowInfo as HalowData)}>{i18n._('common.confirm')}</Button>
                </DialogFooter>
            </DialogContent>
        </Dialog>
    );

    return (
        <div className="mt-2">
            <div className="flex gap-2 justify-between items-center mb-4">
                <Label className="text-sm font-bold text-text-primary">{i18n._('sys.system_management.halow_region')}</Label>
                <Select
                  value={region}
                  onValueChange={handleRegionChange}
                  disabled={!!currentHalowData?.connected}
                >
                    <SelectTrigger className="w-[180px]">
                        <SelectValue placeholder={i18n._('sys.system_management.halow_region')} />
                    </SelectTrigger>
                    <SelectContent>
                        {supportedRegions.map((code) => (
                            <SelectItem key={code} value={code}>
                                {getRegionLabel(code)}
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

                    {!currentHalowData?.connected && otherHalowDataList.length === 0 && (
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

                    {otherHalowDataList.length > 0 && (
                        <div className="mt-4">
                            <p className="text-sm font-bold mb-2">{i18n._('sys.system_management.other_network')}</p>
                            <div className="flex flex-col bg-gray-100 px-4 rounded-lg">
                                {otherHalowDataList.map((item, index) => (
                                    <div onClick={() => { if (isMobile) openConnectDialog(item); }} key={`${item.ssid}-${item.bssid}-${index}`} className="group">
                                        <div className="flex justify-between py-2">
                                            <Label className="text-sm">{item.ssid}</Label>
                                            <div className="flex items-center gap-1">
                                                <Button size="sm" variant="outline" onClick={() => openConnectDialog(item)} className="mr-2 opacity-0 group-hover:opacity-100 transition-opacity">{i18n._('common.connect')}</Button>
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
