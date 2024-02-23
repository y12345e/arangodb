import { AddIcon } from "@chakra-ui/icons";
import {
  Box,
  Button,
  Flex,
  Heading,
  Spinner,
  Stack,
  Stat,
  StatLabel,
  StatNumber
} from "@chakra-ui/react";
import React from "react";
import { useRouteMatch } from "react-router-dom";
import { AddIndexForm } from "./addIndex/AddIndexForm";
import { useCollectionIndicesContext } from "./CollectionIndicesContext";
import { CollectionIndicesTable } from "./listIndices/CollectionIndicesTable";
import { CollectionIndexDetails } from "./viewIndex/CollectionIndexDetails";

export const CollectionIndicesView = () => {
  const { isFormOpen, onOpenForm, onCloseForm } = useCollectionIndicesContext();
  const match = useRouteMatch<{
    indexId: string;
  }>("/cIndices/:collectionName/:indexId");
  const isCollectionDetailsOpen = !!match?.params?.indexId;
  if (isFormOpen) {
    return <AddIndexForm onClose={onCloseForm} />;
  }

  return (
    <Box padding="4" width="100%">
      {isCollectionDetailsOpen && <CollectionIndexDetails />}
      <IndexViewHeader onOpen={onOpenForm} />
      <CollectionIndicesTable />
    </Box>
  );
};

const IndexViewHeader = ({ onOpen }: { onOpen: () => void }) => {
  const { stats } = useCollectionIndicesContext();
  return (
    <Stack direction="row" marginBottom="4" alignItems="baseline" spacing="6">
      <Heading size="lg">Indexes</Heading>
      <Flex gap="2" alignItems="baseline">
        <Stat gap="2">
          <StatNumber lineHeight="base">{stats ? stats.count : <Spinner size="sm" />}</StatNumber>
          <StatLabel lineHeight="base">Index count</StatLabel>
        </Stat>

        <Stat gap="2">
          <StatNumber lineHeight="base">
            {stats ? window.prettyBytes(stats.size) : <Spinner size="sm" />}
          </StatNumber>
          <StatLabel lineHeight="base">Estimated memory used</StatLabel>
        </Stat>
      </Flex>

      <Button
        size="sm"
        leftIcon={<AddIcon />}
        colorScheme="green"
        onClick={() => {
          onOpen();
        }}
      >
        Add index
      </Button>
    </Stack>
  );
};
