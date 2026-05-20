module.exports = {
  root: true,
  env: {
    browser: true,
    es2021: true,
    node: true,
  },
  extends: [
    'airbnb',
    'plugin:@typescript-eslint/recommended',
  ],
  parser: '@typescript-eslint/parser',
  parserOptions: {
    ecmaVersion: 'latest',
    sourceType: 'module',
    project: './tsconfig.app.json',
    tsconfigRootDir: __dirname,
  },
  plugins: [
    '@typescript-eslint',
    'react',
  ],
  rules: {
    // Preact related rules
    'react/jsx-filename-extension': [1, { extensions: ['.tsx', '.ts'] }],
    'react/react-in-jsx-scope': 'off',
    'react/jsx-props-no-spreading': 'off',
    'react/require-default-props': 'off',
    'react/prop-types': 'off',
    'react/button-has-type': 'off',
    'react/no-deprecated': 'off',
    'react/self-closing-comp': 'off',
    'react/jsx-indent': 'off',
    'react/jsx-one-expression-per-line': 'off',
    
    // TypeScript related rules
    '@typescript-eslint/no-unused-vars': ['error', { argsIgnorePattern: '^_' }],
    '@typescript-eslint/no-explicit-any': 'off',
    '@typescript-eslint/explicit-function-return-type': 'off',
    '@typescript-eslint/explicit-module-boundary-types': 'off',
    
    // Import related rules - all disabled to avoid parsing issues
    'import/extensions': 'off',
    'import/prefer-default-export': 'off',
    'import/no-unresolved': 'off',
    'import/no-extraneous-dependencies': 'off',
    'import/no-cycle': 'off',
    'import/named': 'off',
    'import/no-duplicates': 'off',
    'import/order': 'off',
    'import/no-self-import': 'off',
    'import/no-relative-packages': 'off',
    
    // Other rules
    'no-console': 'warn',
    'no-debugger': 'error',
    'prefer-const': 'error',
    'no-var': 'error',
    'object-shorthand': 'error',
    'prefer-template': 'error',
    'no-param-reassign': 'off',
    'no-use-before-define': 'off',
    'jsx-a11y/label-has-associated-control': 'off',
    'quote-props': 'off',
    'comma-dangle': 'off',
    'indent': 'off',
    'linebreak-style': 'off',
    'semi': 'off',
    'quotes': 'off',
    'eol-last': 'off',
    'no-trailing-spaces': 'off',
    'no-multi-spaces': 'off',
    'react/jsx-props-no-multi-spaces': 'off',
    'object-curly-newline': 'off',
    'arrow-parens': 'off',
    'max-len': 'off',
    'no-continue': 'off',
    'no-restricted-syntax': 'off',
    'no-bitwise': 'off',
    // 'class-methods-use-this': 'off',   // Unnecessary check
    'no-nested-ternary': 'off',
    'max-classes-per-file': 'off',
    'no-unused-vars': 'off',  // Nested ternary operators are allowed, this style is convenient and has no major readability issues
    'react/no-array-index-key': 'off',
    'no-plusplus': 'off',
    'consistent-return': 'off',

    'jsx-a11y/click-events-have-key-events': 'off',
    'jsx-a11y/no-static-element-interactions': 'off',
    'jsx-a11y/no-noninteractive-element-interactions': 'off',

    'camelcase': 'off',

  },
  settings: {
    react: {
      version: 'detect',
    },
  },
  overrides: [
    {
      files: ['*.test.ts', '*.test.tsx', '*.spec.ts', '*.spec.tsx'],
      env: {
        jest: true,
      },
      rules: {
        'import/no-extraneous-dependencies': 'off',
      },
    },
    {
      files: [
        'lingui.config.ts',
        'vite.config.ts',
        'vitest.config.ts',
        'vitePlugins/**/*.ts',
        'postcss.config.js',
      ],
      parserOptions: {
        project: null,
      },
    },
  ],
}; 