import * as React from 'react'
import { Button } from '@/components/ui/button'
import { cn } from '@/lib/utils'

interface IconButtonProps extends React.ComponentProps<typeof Button> {
  icon: React.ReactNode
  children: React.ReactNode
}

const IconButton = React.forwardRef<HTMLButtonElement, IconButtonProps>(
  ({ className, icon, children, ...props }, ref) => (
      <Button
        ref={ref}
        className={cn('gap-2', className)}
        {...props}
      >
        <span className="flex-shrink-0">
          {icon}
        </span>
        <span>{children}</span>
      </Button>
    )
)

IconButton.displayName = 'IconButton'

export { IconButton } 