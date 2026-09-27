/** useDetailMaps — live state of the optional high-detail map pack (mapglDetail.ts) for the UI:
 *  installed or not, its real size on disk, and the download in progress. */
import { useEffect, useState } from 'react';
import { detailInstalled, detailSizeBytes, detailState, onDetailState, type DetailState } from './mapglDetail';

export function useDetailMaps(): { installed: boolean; bytes: number; state: DetailState } {
  const read = () => ({ installed: detailInstalled(), bytes: detailSizeBytes(), state: detailState() });
  const [v, setV] = useState(read);
  useEffect(() => onDetailState(() => setV(read())), []);
  return v;
}

/** "169 MB" — whole megabytes, the way a storage line is read. */
export function mb(bytes: number): string {
  return `${Math.round(bytes / 1048576)} MB`;
}
