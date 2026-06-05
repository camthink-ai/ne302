import { useState } from 'preact/hooks';
import { Dialog, DialogContent, DialogHeader, DialogTitle } from '@/components/dialog';
import { Button } from '@/components/ui/button';
import { useLingui } from '@lingui/react';
import { Checkbox } from '@/components/ui/checkbox';
import SvgIcon from '@/components/svg-icon';
import systemApis from '@/services/api/system';
import { toast } from 'sonner';
import Loading from '@/components/loading';
import { createPortal } from 'preact/compat';
import { downloadFile } from '@/utils';

export default function ExportFirmware({ isExportFirmwareDialogOpen, setIsExportFirmwareDialogOpen }: { isExportFirmwareDialogOpen: boolean, setIsExportFirmwareDialogOpen: (open: boolean) => void }) {
    const { i18n } = useLingui();
    const { exportFirmwareReq, exportDeviceFileReq } = systemApis;
    type FirmwareItem = { selected: boolean; filename: string };
    type FirmwareMap = Record<'app' | 'web' | 'device' | 'ai', FirmwareItem>;
    const [loading, setLoading] = useState(false);
    const [fireWares, setFireWares] = useState<FirmwareMap>({
        app: { selected: false, filename: 'ne302_Appli.bin' },
        web: { selected: false, filename: 'ne302_web.bin' },
        device: { selected: false, filename: 'ne302_device.json' },
        ai: { selected: false, filename: 'ne302_ai_model.bin' }
    });

    const handleExportFirmware = async () => {
        try {
            const keys = Object.keys(fireWares) as Array<keyof typeof fireWares>;
            const selectedFirmwares = keys.filter((key) => fireWares[key].selected);
            if (selectedFirmwares.length === 0) {
                toast.error(i18n._('sys.system_management.please_select_firmware'));
                return;
            }
            setLoading(true);
            /* eslint-disable no-await-in-loop */
            for (const key of selectedFirmwares) {
                if (key === 'device') {
                    const res = await exportDeviceFileReq();
                    const file = JSON.stringify(res.data);
                    await downloadFile(file, fireWares[key].filename);
                } else {
                    const res = await exportFirmwareReq({
                        firmware_type: key,
                        filename: fireWares[key].filename,
                    });
                    await downloadFile(res as any, fireWares[key].filename);
                }
            }
            /* eslint-enable no-await-in-loop */
            Object.keys(fireWares).forEach((key) => {
                if (fireWares[key as keyof typeof fireWares].selected) {
                    fireWares[key as keyof typeof fireWares].selected = false;
                }
            })
        } catch (error) {
            console.error(error);
        } finally {
            setLoading(false);
        }
    }

    const LoadingOverlay = () => createPortal(
        <div className="fixed inset-0 bg-black/40 backdrop-blur-sm flex items-center justify-center z-[9999]">
            <Loading placeholder={i18n._('sys.system_management.exporting_firmware')} />
        </div>,
        document.body
    );

    return (
        <div>
            {loading && <LoadingOverlay />}
            <Dialog open={isExportFirmwareDialogOpen} onOpenChange={setIsExportFirmwareDialogOpen}>
                <DialogContent className="md:max-w-2xl mx-4">
                    <DialogHeader>
                        <DialogTitle>{i18n._('sys.system_management.export_firmware')} </DialogTitle>
                    </DialogHeader>
                    <div className="flex flex-col gap-2 flex-wrap">
                        <div className="md:flex md:flex-wrap gap-4 md:justify-center grid grid-cols-2 mb-2 mt-4">
                            {(Object.keys(fireWares) as Array<keyof typeof fireWares>).map((key) => (
                                <div className=" flex flex-col min-w-26 gap-2 items-center justify-center" key={key as string}>
                                    <div className="relative">
                                        <Checkbox className="absolute -top-2 -left-2 data-[state=checked]:bg-neutral data-[state=checked]:text-white" checked={fireWares[key].selected} onCheckedChange={() => setFireWares({ ...fireWares, [key]: { ...fireWares[key], selected: !fireWares[key].selected } })} />
                                        <div className="w-16 h-16 bg-gray-400 rounded-md flex items-center justify-center cursor-pointer" onClick={() => setFireWares({ ...fireWares, [key]: { ...fireWares[key], selected: !fireWares[key].selected } })}>
                                            <SvgIcon className="w-10 h-10" icon="file" />
                                        </div>
                                    </div>
                                    <p className="text-sm text-wrap">{fireWares[key].filename}</p>
                                </div>
                            ))}
                        </div>
                        <div className="flex gap-2 justify-end">
                            <Button className="w-1/2 md:w-auto" variant="outline" onClick={() => setIsExportFirmwareDialogOpen(false)}>{i18n._('common.cancel')}</Button>
                            <Button className="w-1/2 md:w-auto" variant="primary" onClick={() => handleExportFirmware()}>{i18n._('sys.system_management.export_firmware')}</Button>
                        </div>
                    </div>
                </DialogContent>
            </Dialog>
        </div>
    )
}