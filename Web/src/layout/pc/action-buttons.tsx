import { useState } from 'preact/hooks';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { Link } from 'react-router-dom'
import { type LocaleType } from '@/locales'
import { useLanguage } from '@/hooks/useLanguage'
import SvgIcon from '@/components/svg-icon'
import { useAuthStore } from '@/store/auth';

import { Button } from "@/components/ui/button"
import Log from '../log'

export default function ActionButtons() {
  const { locale, setLocale } = useLanguage()
  const { isValidateToken } = useAuthStore();
  const handleLanguageChange = (value: string) => {
    setLocale(value as LocaleType);
  }
  const [isOpen, setIsOpen] = useState(false);

  return (
    <>
      {isValidateToken && (
        <Link to="https://wiki.camthink.ai/docs/neoeyes-ne302-series/overview" className="mx-2" target="_blank">
          <Button variant="outline" size="icon" className="w-9 h-9">
            <SvgIcon icon="hint" />
          </Button>
        </Link>
      )}

      {/* GitHub button */}
      {isValidateToken && (
        <>
          <Link to="https://github.com/camthink-ai" className="mx-0" target="_blank">
            <Button variant="outline" size="icon" className="w-9 h-9">
              <SvgIcon icon="github" />
            </Button>
          </Link>

          <div className="w-[1px] h-8 bg-gray-200 mx-3"></div>
        </>
      )}
      {/* Language switch */}
      <Select
        value={locale}
        onValueChange={handleLanguageChange}
      >
        <SelectTrigger className="w-[120px] bg-white">
          <SelectValue />
        </SelectTrigger>
        <SelectContent>
          <SelectItem value="zh">
            简体中文
          </SelectItem>
          <SelectItem value="en">
            English
          </SelectItem>
        </SelectContent>
      </Select>
      {isValidateToken && (
        <>
          <Button variant="outline" size="icon" className="w-9 h-9 bg-white" onClick={() => setIsOpen(true)}>
            <SvgIcon className="text-text-primary" icon="logs" />
          </Button>
          <Log isOpen={isOpen} setOpen={setIsOpen} />
        </>
      )}
    </>
  )
}