-- Candidate fragments embedded by identity.sh and timing.sh.
-- Full candidate SQL is materialized into host scratch by each script before execution.

-- B1: WithItemLinkItemIds UNION becomes UNION ALL.
WithItemLinkItemIds AS (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (6,4,7,10,3,2,0,1,5) union all select ItemLinks2TwoLevel.LinkedId from ItemLinks2 ItemLinks2TwoLevel where itemid in (select ItemLinks2.LinkedId From ItemLinks2 join withancestors on withancestors.itemid=itemlinks2.itemid  where ItemLinks2.Type in (7,0,1,5,6,4,2)))

-- B2: itemPeople2 IN becomes EXISTS.
EXISTS (SELECT 1 FROM itemPeople2 WHERE itemPeople2.PersonId = A.Id AND itemPeople2.ItemId in WithAncestors)

-- B3: ListItems exemption IN becomes EXISTS.
EXISTS (SELECT 1 FROM ListItems ListItemsExemptionForPlaylists JOIN ancestorids2 AncestorIdExemptionForPlaylists ON ListItemsExemptionForPlaylists.ListItemId=AncestorIdExemptionForPlaylists.itemid AND AncestorIdExemptionForPlaylists.AncestorId in (__ANCESTORS__) WHERE ListItemsExemptionForPlaylists.ListId = A.Id)

-- B4: WithAncestors membership becomes EXISTS.
EXISTS (SELECT 1 FROM WithAncestors WHERE WithAncestors.itemid = A.Id)

-- B5: WithItemLinkItemIds membership becomes EXISTS.
EXISTS (SELECT 1 FROM WithItemLinkItemIds WHERE WithItemLinkItemIds.LinkedId = A.Id)

-- B5_INLINE: delete WithItemLinkItemIds and replace the arm with two correlated EXISTS branches.
(EXISTS (SELECT 1 FROM ItemLinks2 JOIN WithAncestors ON WithAncestors.itemid = ItemLinks2.itemid WHERE ItemLinks2.LinkedId = A.Id AND ItemLinks2.Type in (6,4,7,10,3,2,0,1,5)) OR EXISTS (SELECT 1 FROM ItemLinks2 ItemLinks2TwoLevel JOIN ItemLinks2 ItemLinks2Seed ON ItemLinks2Seed.LinkedId = ItemLinks2TwoLevel.itemid JOIN WithAncestors ON WithAncestors.itemid = ItemLinks2Seed.itemid WHERE ItemLinks2TwoLevel.LinkedId = A.Id AND ItemLinks2Seed.Type in (7,0,1,5,6,4,2)))

-- B7: both CTEs use AS NOT MATERIALIZED.
WithAncestors AS NOT MATERIALIZED (
WithItemLinkItemIds AS NOT MATERIALIZED (

-- B8: materialize FTS matches first, then join mediaitems.
,FtsCandidates AS MATERIALIZED (SELECT rowid AS RowId, rank AS Rank FROM fts_search9 WHERE fts_search9 MATCH '__MATCH_LITERAL__')
from FtsCandidates join mediaitems A on A.Id=FtsCandidates.RowId

-- COMBINED: B1+B2+B3+B4+B5+B7, using B5 direct so both CTEs remain available for B7.
