import { useState, useEffect, useCallback } from 'preact/hooks';
import { useLingui } from '@lingui/react';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Skeleton } from '@/components/ui/skeleton';
import { Button } from '@/components/ui/button';
import { Dialog, DialogContent, DialogHeader, DialogTitle, DialogFooter } from '@/components/dialog';
import { toast } from 'sonner';
import systemSettings from '@/services/api/systemSettings';

interface UsbStatus {
    enabled: boolean;
    running: boolean;
    rndis_active: boolean;
}

/** Device IP of the RNDIS netif ("ud"): browsing through the USB NIC means
 *  turning USB off also cuts THIS page's network. */
const USB_RNDIS_HOST = '192.168.20.1';

/** USB section embedded in the "Device Configuration" page (device-password.tsx):
 *  master switch for the USB device (CDC console + RNDIS NIC). Toggling
 *  applies on the device immediately — no Save button, like other instant
 *  switches (e.g. fill-light in hardware management). */
export default function UsbSettings() {
    const { i18n } = useLingui();
    const { getUsbStatusReq, setUsbConfigReq } = systemSettings;
    const [loading, setLoading] = useState(true);
    const [status, setStatus] = useState<UsbStatus | null>(null);
    const [switching, setSwitching] = useState(false);
    const [isConfirmOffDialogOpen, setIsConfirmOffDialogOpen] = useState(false);

    const loadStatus = useCallback(async () => {
        try {
            setLoading(true);
            const res = await getUsbStatusReq();
            setStatus({
                enabled: !!res.data.enabled,
                running: !!res.data.running,
                rndis_active: !!res.data.rndis_active,
            });
        } catch (error) {
            console.error('getUsbStatus', error);
        } finally {
            setLoading(false);
        }
    }, []);

    useEffect(() => {
        loadStatus();
    }, [loadStatus]);

    const applyToggle = async (checked: boolean) => {
        if (!status || switching) return;
        const prev = status;
        // optimistic: flip immediately, restore on failure
        setStatus({ ...status, enabled: checked });
        setSwitching(true);

        // Disabling while THIS page rides the USB RNDIS link: the device
        // tears the NIC down synchronously inside the HTTP handler, so the
        // request may die mid-flight — a network error there IS the success
        // confirmation (skip the global error toast for it).
        const viaRndis = window.location.hostname === USB_RNDIS_HOST;
        const reqCfg = (!checked && viaRndis) ? { skipErrorToast: true } : undefined;

        try {
            const res = await setUsbConfigReq({ enabled: checked }, reqCfg);
            setStatus({
                enabled: !!res.data.enabled,
                running: !!res.data.running,
                rndis_active: prev.rndis_active,
            });
            toast.success(checked
                ? i18n._('sys.system_management.usb_enable_ok')
                : i18n._('sys.system_management.usb_disable_ok'));
        } catch (error) {
            console.error('setUsbConfig', error);
            if (!checked && viaRndis) {
                // expected outcome: the NIC that carried this request is gone
                setStatus({ enabled: false, running: false, rndis_active: false });
                toast.success(i18n._('sys.system_management.usb_disable_ok'));
            } else {
                setStatus(prev);
            }
        } finally {
            setSwitching(false);
        }
    };

    const handleToggle = (checked: boolean) => {
        // Disabling while THIS page rides the USB RNDIS link: confirm first —
        // the connection dies with the NIC and the user must switch to the
        // device hotspot (192.168.10.10) to get back.
        if (!checked && window.location.hostname === USB_RNDIS_HOST) {
            setIsConfirmOffDialogOpen(true);
            return;
        }
        applyToggle(checked);
    };

    const confirmOffDialog = () => (
        <Dialog open={isConfirmOffDialogOpen} onOpenChange={setIsConfirmOffDialogOpen}>
            <DialogContent className="mx-8">
                <DialogHeader>
                    <DialogTitle>{i18n._('sys.system_management.usb_off_via_rndis_title')}</DialogTitle>
                    <div className="text-sm text-text-primary my-4">{i18n._('sys.system_management.usb_off_via_rndis_body')}</div>
                </DialogHeader>
                <DialogFooter>
                    <Button className="md:w-auto w-1/2" variant="outline" onClick={() => setIsConfirmOffDialogOpen(false)}>{i18n._('common.cancel')}</Button>
                    <Button className="md:w-auto w-1/2" variant="primary" onClick={() => { setIsConfirmOffDialogOpen(false); applyToggle(false); }}>{i18n._('sys.system_management.usb_off_confirm')}</Button>
                </DialogFooter>
            </DialogContent>
        </Dialog>
    );

    if (loading) {
        return (
            <>
                <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                    {i18n._('sys.system_management.usb_title')}
                </p>
                <div className="bg-gray-100 p-4 rounded-lg">
                    <Skeleton className="h-10 w-full" />
                </div>
            </>
        );
    }

    if (!status) {
        return (
            <>
                <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                    {i18n._('sys.system_management.usb_title')}
                </p>
                <div className="bg-gray-100 p-4 rounded-lg">
                    <p className="text-sm text-gray-400">{i18n._('sys.system_management.usb_load_failed')}</p>
                </div>
            </>
        );
    }

    return (
        <>
            <p className="text-sm text-text-primary font-bold mt-4 mb-2">
                {i18n._('sys.system_management.usb_title')}
            </p>
            <div className="flex flex-col gap-2 bg-gray-100 p-4 rounded-lg">
                <div className="flex justify-between items-center">
                    <div className="flex flex-col gap-1 max-w-[70%]">
                        <Label className="text-sm text-text-primary shrink-0">{i18n._('sys.system_management.usb_switch')}</Label>
                        <p className="text-xs text-gray-400">{i18n._('sys.system_management.usb_switch_hint')}</p>
                        <p className={`text-xs font-medium ${status.running ? 'text-green-600' : 'text-gray-500'}`}>
                            {status.running
                                ? i18n._('sys.system_management.usb_running')
                                : i18n._('sys.system_management.usb_stopped')}
                        </p>
                    </div>
                    <Switch
                      checked={status.enabled}
                      onCheckedChange={handleToggle}
                      disabled={switching}
                    />
                </div>
            </div>
            {confirmOffDialog()}
        </>
    );
}
