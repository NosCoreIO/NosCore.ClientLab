import {Console} from 'node:console';

export const logger = new Console({stdout:process.stdout, stderr:process.stderr});
