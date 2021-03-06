/*
 * BFQ: Hierarchical B-WF2Q+ scheduler.
 *
 * Based on ideas and code from CFQ:
 * Copyright (C) 2003 Jens Axboe <axboe@kernel.dk>
 *
 * Copyright (C) 2008 Fabio Checconi <fabio@gandalf.sssup.it>
 *		      Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2015 Paolo Valente <paolo.valente@unimore.it>
 *
 * Copyright (C) 2016 Paolo Valente <paolo.valente@linaro.org>
 */

static struct bfq_group *bfqq_group(struct bfq_queue *bfqq);

#ifdef CONFIG_BFQ_GROUP_IOSCHED
#define for_each_entity(entity)	\
	for (; entity ; entity = entity->parent)

#define for_each_entity_safe(entity, parent) \
	for (; entity && ({ parent = entity->parent; 1; }); entity = parent)


static struct bfq_entity *bfq_lookup_next_entity(struct bfq_sched_data *sd,
						 int extract,
						 struct bfq_data *bfqd);

static void bfq_update_budget(struct bfq_entity *next_in_service)
{
	struct bfq_entity *bfqg_entity;
	struct bfq_group *bfqg;
	struct bfq_sched_data *group_sd;

	BUG_ON(!next_in_service);

	group_sd = next_in_service->sched_data;

	bfqg = container_of(group_sd, struct bfq_group, sched_data);
	/*
	 * bfq_group's my_entity field is not NULL only if the group
	 * is not the root group. We must not touch the root entity
	 * as it must never become an in-service entity.
	 */
	bfqg_entity = bfqg->my_entity;
	if (bfqg_entity)
		bfqg_entity->budget = next_in_service->budget;
}

static int bfq_update_next_in_service(struct bfq_sched_data *sd)
{
	struct bfq_entity *next_in_service;
	struct bfq_queue *bfqq;

	if (sd->in_service_entity)
		/* will update/requeue at the end of service */
		return 0;

	/*
	 * NOTE: this can be improved in many ways, such as returning
	 * 1 (and thus propagating upwards the update) only when the
	 * budget changes, or caching the bfqq that will be scheduled
	 * next from this subtree.  By now we worry more about
	 * correctness than about performance...
	 */
	next_in_service = bfq_lookup_next_entity(sd, 0, NULL);
	sd->next_in_service = next_in_service;

	if (next_in_service)
		bfq_update_budget(next_in_service);
	else
		goto exit;

	bfqq = bfq_entity_to_bfqq(next_in_service);
	if (bfqq)
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			     "update_next_in_service: chosen this queue");
	else {
		struct bfq_group *bfqg =
			container_of(next_in_service,
				     struct bfq_group, entity);

		bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
			     "update_next_in_service: chosen this entity");
	}
exit:
	return 1;
}

static void bfq_check_next_in_service(struct bfq_sched_data *sd,
				      struct bfq_entity *entity)
{
	WARN_ON(sd->next_in_service != entity);
}
#else
#define for_each_entity(entity)	\
	for (; entity ; entity = NULL)

#define for_each_entity_safe(entity, parent) \
	for (parent = NULL; entity ; entity = parent)

static int bfq_update_next_in_service(struct bfq_sched_data *sd)
{
	return 0;
}

static void bfq_check_next_in_service(struct bfq_sched_data *sd,
				      struct bfq_entity *entity)
{
}

static void bfq_update_budget(struct bfq_entity *next_in_service)
{
}
#endif

/*
 * Shift for timestamp calculations.  This actually limits the maximum
 * service allowed in one timestamp delta (small shift values increase it),
 * the maximum total weight that can be used for the queues in the system
 * (big shift values increase it), and the period of virtual time
 * wraparounds.
 */
#define WFQ_SERVICE_SHIFT	22

/**
 * bfq_gt - compare two timestamps.
 * @a: first ts.
 * @b: second ts.
 *
 * Return @a > @b, dealing with wrapping correctly.
 */
static int bfq_gt(u64 a, u64 b)
{
	return (s64)(a - b) > 0;
}

static struct bfq_queue *bfq_entity_to_bfqq(struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = NULL;

	BUG_ON(!entity);

	if (!entity->my_sched_data)
		bfqq = container_of(entity, struct bfq_queue, entity);

	return bfqq;
}


/**
 * bfq_delta - map service into the virtual time domain.
 * @service: amount of service.
 * @weight: scale factor (weight of an entity or weight sum).
 */
static u64 bfq_delta(unsigned long service, unsigned long weight)
{
	u64 d = (u64)service << WFQ_SERVICE_SHIFT;

	do_div(d, weight);
	return d;
}

/**
 * bfq_calc_finish - assign the finish time to an entity.
 * @entity: the entity to act upon.
 * @service: the service to be charged to the entity.
 */
static void bfq_calc_finish(struct bfq_entity *entity, unsigned long service)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	unsigned long long start, finish, delta;

	BUG_ON(entity->weight == 0);

	entity->finish = entity->start +
		bfq_delta(service, entity->weight);

	start = ((entity->start>>10)*1000)>>12;
	finish = ((entity->finish>>10)*1000)>>12;
	delta = ((bfq_delta(service, entity->weight)>>10)*1000)>>12;

	if (bfqq) {
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			"calc_finish: serv %lu, w %d",
			service, entity->weight);
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			"calc_finish: start %llu, finish %llu, delta %llu",
			start, finish, delta);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	} else {
		struct bfq_group *bfqg =
			container_of(entity, struct bfq_group, entity);

		bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
			"calc_finish group: serv %lu, w %d",
			     service, entity->weight);
		bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
			"calc_finish group: start %llu, finish %llu, delta %llu",
			start, finish, delta);
#endif
	}
}

/**
 * bfq_entity_of - get an entity from a node.
 * @node: the node field of the entity.
 *
 * Convert a node pointer to the relative entity.  This is used only
 * to simplify the logic of some functions and not as the generic
 * conversion mechanism because, e.g., in the tree walking functions,
 * the check for a %NULL value would be redundant.
 */
static struct bfq_entity *bfq_entity_of(struct rb_node *node)
{
	struct bfq_entity *entity = NULL;

	if (node)
		entity = rb_entry(node, struct bfq_entity, rb_node);

	return entity;
}

/**
 * bfq_extract - remove an entity from a tree.
 * @root: the tree root.
 * @entity: the entity to remove.
 */
static void bfq_extract(struct rb_root *root, struct bfq_entity *entity)
{
	BUG_ON(entity->tree != root);

	entity->tree = NULL;
	rb_erase(&entity->rb_node, root);
}

/**
 * bfq_idle_extract - extract an entity from the idle tree.
 * @st: the service tree of the owning @entity.
 * @entity: the entity being removed.
 */
static void bfq_idle_extract(struct bfq_service_tree *st,
			     struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	struct rb_node *next;

	BUG_ON(entity->tree != &st->idle);

	if (entity == st->first_idle) {
		next = rb_next(&entity->rb_node);
		st->first_idle = bfq_entity_of(next);
	}

	if (entity == st->last_idle) {
		next = rb_prev(&entity->rb_node);
		st->last_idle = bfq_entity_of(next);
	}

	bfq_extract(&st->idle, entity);

	if (bfqq)
		list_del(&bfqq->bfqq_list);
}

/**
 * bfq_insert - generic tree insertion.
 * @root: tree root.
 * @entity: entity to insert.
 *
 * This is used for the idle and the active tree, since they are both
 * ordered by finish time.
 */
static void bfq_insert(struct rb_root *root, struct bfq_entity *entity)
{
	struct bfq_entity *entry;
	struct rb_node **node = &root->rb_node;
	struct rb_node *parent = NULL;

	BUG_ON(entity->tree);

	while (*node) {
		parent = *node;
		entry = rb_entry(parent, struct bfq_entity, rb_node);

		if (bfq_gt(entry->finish, entity->finish))
			node = &parent->rb_left;
		else
			node = &parent->rb_right;
	}

	rb_link_node(&entity->rb_node, parent, node);
	rb_insert_color(&entity->rb_node, root);

	entity->tree = root;
}

/**
 * bfq_update_min - update the min_start field of a entity.
 * @entity: the entity to update.
 * @node: one of its children.
 *
 * This function is called when @entity may store an invalid value for
 * min_start due to updates to the active tree.  The function  assumes
 * that the subtree rooted at @node (which may be its left or its right
 * child) has a valid min_start value.
 */
static void bfq_update_min(struct bfq_entity *entity, struct rb_node *node)
{
	struct bfq_entity *child;

	if (node) {
		child = rb_entry(node, struct bfq_entity, rb_node);
		if (bfq_gt(entity->min_start, child->min_start))
			entity->min_start = child->min_start;
	}
}

/**
 * bfq_update_active_node - recalculate min_start.
 * @node: the node to update.
 *
 * @node may have changed position or one of its children may have moved,
 * this function updates its min_start value.  The left and right subtrees
 * are assumed to hold a correct min_start value.
 */
static void bfq_update_active_node(struct rb_node *node)
{
	struct bfq_entity *entity = rb_entry(node, struct bfq_entity, rb_node);
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	entity->min_start = entity->start;
	bfq_update_min(entity, node->rb_right);
	bfq_update_min(entity, node->rb_left);

	if (bfqq) {
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			     "update_active_node: new min_start %llu",
			     ((entity->min_start>>10)*1000)>>12);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	} else {
		struct bfq_group *bfqg =
			container_of(entity, struct bfq_group, entity);

		bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
			     "update_active_node: new min_start %llu",
			     ((entity->min_start>>10)*1000)>>12);
#endif
	}
}

/**
 * bfq_update_active_tree - update min_start for the whole active tree.
 * @node: the starting node.
 *
 * @node must be the deepest modified node after an update.  This function
 * updates its min_start using the values held by its children, assuming
 * that they did not change, and then updates all the nodes that may have
 * changed in the path to the root.  The only nodes that may have changed
 * are the ones in the path or their siblings.
 */
static void bfq_update_active_tree(struct rb_node *node)
{
	struct rb_node *parent;

up:
	bfq_update_active_node(node);

	parent = rb_parent(node);
	if (!parent)
		return;

	if (node == parent->rb_left && parent->rb_right)
		bfq_update_active_node(parent->rb_right);
	else if (parent->rb_left)
		bfq_update_active_node(parent->rb_left);

	node = parent;
	goto up;
}

static void bfq_weights_tree_add(struct bfq_data *bfqd,
				 struct bfq_entity *entity,
				 struct rb_root *root);

static void bfq_weights_tree_remove(struct bfq_data *bfqd,
				    struct bfq_entity *entity,
				    struct rb_root *root);


/**
 * bfq_active_insert - insert an entity in the active tree of its
 *                     group/device.
 * @st: the service tree of the entity.
 * @entity: the entity being inserted.
 *
 * The active tree is ordered by finish time, but an extra key is kept
 * per each node, containing the minimum value for the start times of
 * its children (and the node itself), so it's possible to search for
 * the eligible node with the lowest finish time in logarithmic time.
 */
static void bfq_active_insert(struct bfq_service_tree *st,
			      struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	struct rb_node *node = &entity->rb_node;
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	struct bfq_sched_data *sd = NULL;
	struct bfq_group *bfqg = NULL;
	struct bfq_data *bfqd = NULL;
#endif

	bfq_insert(&st->active, entity);

	if (node->rb_left)
		node = node->rb_left;
	else if (node->rb_right)
		node = node->rb_right;

	bfq_update_active_tree(node);

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	sd = entity->sched_data;
	bfqg = container_of(sd, struct bfq_group, sched_data);
	BUG_ON(!bfqg);
	bfqd = (struct bfq_data *)bfqg->bfqd;
#endif
	if (bfqq)
		list_add(&bfqq->bfqq_list, &bfqq->bfqd->active_list);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	else { /* bfq_group */
		BUG_ON(!bfqd);
		bfq_weights_tree_add(bfqd, entity, &bfqd->group_weights_tree);
	}
	if (bfqg != bfqd->root_group) {
		BUG_ON(!bfqg);
		BUG_ON(!bfqd);
		bfqg->active_entities++;
	}
#endif
}

/**
 * bfq_ioprio_to_weight - calc a weight from an ioprio.
 * @ioprio: the ioprio value to convert.
 */
static unsigned short bfq_ioprio_to_weight(int ioprio)
{
	BUG_ON(ioprio < 0 || ioprio >= IOPRIO_BE_NR);
	return (IOPRIO_BE_NR - ioprio) * BFQ_WEIGHT_CONVERSION_COEFF;
}

/**
 * bfq_weight_to_ioprio - calc an ioprio from a weight.
 * @weight: the weight value to convert.
 *
 * To preserve as much as possible the old only-ioprio user interface,
 * 0 is used as an escape ioprio value for weights (numerically) equal or
 * larger than IOPRIO_BE_NR * BFQ_WEIGHT_CONVERSION_COEFF.
 */
static unsigned short bfq_weight_to_ioprio(int weight)
{
	BUG_ON(weight < BFQ_MIN_WEIGHT || weight > BFQ_MAX_WEIGHT);
	return IOPRIO_BE_NR * BFQ_WEIGHT_CONVERSION_COEFF - weight < 0 ?
		0 : IOPRIO_BE_NR * BFQ_WEIGHT_CONVERSION_COEFF - weight;
}

static void bfq_get_entity(struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

	if (bfqq) {
		bfqq->ref++;
		bfq_log_bfqq(bfqq->bfqd, bfqq, "get_entity: %p %d",
			     bfqq, bfqq->ref);
	}
}

/**
 * bfq_find_deepest - find the deepest node that an extraction can modify.
 * @node: the node being removed.
 *
 * Do the first step of an extraction in an rb tree, looking for the
 * node that will replace @node, and returning the deepest node that
 * the following modifications to the tree can touch.  If @node is the
 * last node in the tree return %NULL.
 */
static struct rb_node *bfq_find_deepest(struct rb_node *node)
{
	struct rb_node *deepest;

	if (!node->rb_right && !node->rb_left)
		deepest = rb_parent(node);
	else if (!node->rb_right)
		deepest = node->rb_left;
	else if (!node->rb_left)
		deepest = node->rb_right;
	else {
		deepest = rb_next(node);
		if (deepest->rb_right)
			deepest = deepest->rb_right;
		else if (rb_parent(deepest) != node)
			deepest = rb_parent(deepest);
	}

	return deepest;
}

/**
 * bfq_active_extract - remove an entity from the active tree.
 * @st: the service_tree containing the tree.
 * @entity: the entity being removed.
 */
static void bfq_active_extract(struct bfq_service_tree *st,
			       struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	struct rb_node *node;
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	struct bfq_sched_data *sd = NULL;
	struct bfq_group *bfqg = NULL;
	struct bfq_data *bfqd = NULL;
#endif

	node = bfq_find_deepest(&entity->rb_node);
	bfq_extract(&st->active, entity);

	if (node)
		bfq_update_active_tree(node);

#ifdef CONFIG_BFQ_GROUP_IOSCHED
	sd = entity->sched_data;
	bfqg = container_of(sd, struct bfq_group, sched_data);
	BUG_ON(!bfqg);
	bfqd = (struct bfq_data *)bfqg->bfqd;
#endif
	if (bfqq)
		list_del(&bfqq->bfqq_list);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	else { /* bfq_group */
		BUG_ON(!bfqd);
		bfq_weights_tree_remove(bfqd, entity,
					&bfqd->group_weights_tree);
	}
	if (bfqg != bfqd->root_group) {
		BUG_ON(!bfqg);
		BUG_ON(!bfqd);
		BUG_ON(!bfqg->active_entities);
		bfqg->active_entities--;
	}
#endif
}

/**
 * bfq_idle_insert - insert an entity into the idle tree.
 * @st: the service tree containing the tree.
 * @entity: the entity to insert.
 */
static void bfq_idle_insert(struct bfq_service_tree *st,
			    struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	struct bfq_entity *first_idle = st->first_idle;
	struct bfq_entity *last_idle = st->last_idle;

	if (!first_idle || bfq_gt(first_idle->finish, entity->finish))
		st->first_idle = entity;
	if (!last_idle || bfq_gt(entity->finish, last_idle->finish))
		st->last_idle = entity;

	bfq_insert(&st->idle, entity);

	if (bfqq)
		list_add(&bfqq->bfqq_list, &bfqq->bfqd->idle_list);
}

/**
 * bfq_forget_entity - remove an entity from the wfq trees.
 * @st: the service tree.
 * @entity: the entity being removed.
 *
 * Update the device status and forget everything about @entity, putting
 * the device reference to it, if it is a queue.  Entities belonging to
 * groups are not refcounted.
 */
static void bfq_forget_entity(struct bfq_service_tree *st,
			      struct bfq_entity *entity)
{
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	struct bfq_sched_data *sd;

	BUG_ON(!entity->on_st);

	entity->on_st = 0;
	st->wsum -= entity->weight;
	if (bfqq) {
		sd = entity->sched_data;
		bfq_log_bfqq(bfqq->bfqd, bfqq, "forget_entity: %p %d",
			     bfqq, bfqq->ref);
		bfq_put_queue(bfqq);
	}
}

/**
 * bfq_put_idle_entity - release the idle tree ref of an entity.
 * @st: service tree for the entity.
 * @entity: the entity being released.
 */
static void bfq_put_idle_entity(struct bfq_service_tree *st,
				struct bfq_entity *entity)
{
	bfq_idle_extract(st, entity);
	bfq_forget_entity(st, entity);
}

/**
 * bfq_forget_idle - update the idle tree if necessary.
 * @st: the service tree to act upon.
 *
 * To preserve the global O(log N) complexity we only remove one entry here;
 * as the idle tree will not grow indefinitely this can be done safely.
 */
static void bfq_forget_idle(struct bfq_service_tree *st)
{
	struct bfq_entity *first_idle = st->first_idle;
	struct bfq_entity *last_idle = st->last_idle;

	if (RB_EMPTY_ROOT(&st->active) && last_idle &&
	    !bfq_gt(last_idle->finish, st->vtime)) {
		/*
		 * Forget the whole idle tree, increasing the vtime past
		 * the last finish time of idle entities.
		 */
		st->vtime = last_idle->finish;
	}

	if (first_idle && !bfq_gt(first_idle->finish, st->vtime))
		bfq_put_idle_entity(st, first_idle);
}

static struct bfq_service_tree *
__bfq_entity_update_weight_prio(struct bfq_service_tree *old_st,
			 struct bfq_entity *entity)
{
	struct bfq_service_tree *new_st = old_st;

	if (entity->prio_changed) {
		struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
		unsigned int prev_weight, new_weight;
		struct bfq_data *bfqd = NULL;
		struct rb_root *root;
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		struct bfq_sched_data *sd;
		struct bfq_group *bfqg;
#endif

		if (bfqq)
			bfqd = bfqq->bfqd;
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		else {
			sd = entity->my_sched_data;
			bfqg = container_of(sd, struct bfq_group, sched_data);
			BUG_ON(!bfqg);
			bfqd = (struct bfq_data *)bfqg->bfqd;
			BUG_ON(!bfqd);
		}
#endif

		BUG_ON(old_st->wsum < entity->weight);
		old_st->wsum -= entity->weight;

		if (entity->new_weight != entity->orig_weight) {
			if (entity->new_weight < BFQ_MIN_WEIGHT ||
			    entity->new_weight > BFQ_MAX_WEIGHT) {
				pr_crit("update_weight_prio: new_weight %d\n",
					entity->new_weight);
				if (entity->new_weight < BFQ_MIN_WEIGHT)
					entity->new_weight = BFQ_MIN_WEIGHT;
				else
					entity->new_weight = BFQ_MAX_WEIGHT;
			}
			entity->orig_weight = entity->new_weight;
			if (bfqq)
				bfqq->ioprio =
				  bfq_weight_to_ioprio(entity->orig_weight);
		}

		if (bfqq)
			bfqq->ioprio_class = bfqq->new_ioprio_class;
		entity->prio_changed = 0;

		/*
		 * NOTE: here we may be changing the weight too early,
		 * this will cause unfairness.  The correct approach
		 * would have required additional complexity to defer
		 * weight changes to the proper time instants (i.e.,
		 * when entity->finish <= old_st->vtime).
		 */
		new_st = bfq_entity_service_tree(entity);

		prev_weight = entity->weight;
		new_weight = entity->orig_weight *
			     (bfqq ? bfqq->wr_coeff : 1);
		/*
		 * If the weight of the entity changes, remove the entity
		 * from its old weight counter (if there is a counter
		 * associated with the entity), and add it to the counter
		 * associated with its new weight.
		 */
		if (prev_weight != new_weight) {
			if (bfqq)
				bfq_log_bfqq(bfqq->bfqd, bfqq,
					     "weight changed %d %d(%d %d)",
					     prev_weight, new_weight,
					     entity->orig_weight,
					     bfqq->wr_coeff);

			root = bfqq ? &bfqd->queue_weights_tree :
				      &bfqd->group_weights_tree;
			bfq_weights_tree_remove(bfqd, entity, root);
		}
		entity->weight = new_weight;
		/*
		 * Add the entity to its weights tree only if it is
		 * not associated with a weight-raised queue.
		 */
		if (prev_weight != new_weight &&
		    (bfqq ? bfqq->wr_coeff == 1 : 1))
			/* If we get here, root has been initialized. */
			bfq_weights_tree_add(bfqd, entity, root);

		new_st->wsum += entity->weight;

		if (new_st != old_st)
			entity->start = new_st->vtime;
	}

	return new_st;
}

#ifdef CONFIG_BFQ_GROUP_IOSCHED
static void bfqg_stats_set_start_empty_time(struct bfq_group *bfqg);
#endif

/**
 * bfq_bfqq_served - update the scheduler status after selection for
 *                   service.
 * @bfqq: the queue being served.
 * @served: bytes to transfer.
 *
 * NOTE: this can be optimized, as the timestamps of upper level entities
 * are synchronized every time a new bfqq is selected for service.  By now,
 * we keep it to better check consistency.
 */
static void bfq_bfqq_served(struct bfq_queue *bfqq, int served)
{
	struct bfq_entity *entity = &bfqq->entity;
	struct bfq_service_tree *st;

	for_each_entity(entity) {
		st = bfq_entity_service_tree(entity);

		entity->service += served;

		BUG_ON(st->wsum == 0);

		st->vtime += bfq_delta(served, st->wsum);
		bfq_forget_idle(st);
	}
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	bfqg_stats_set_start_empty_time(bfqq_group(bfqq));
#endif
	st = bfq_entity_service_tree(&bfqq->entity);
	bfq_log_bfqq(bfqq->bfqd, bfqq, "bfqq_served %d secs, vtime %llu on %p",
		     served,  ((st->vtime>>10)*1000)>>12, st);
}

/**
 * bfq_bfqq_charge_time - charge an amount of service equivalent to the length
 *			  of the time interval during which bfqq has been in
 *			  service.
 * @bfqd: the device
 * @bfqq: the queue that needs a service update.
 * @time_ms: the amount of time during which the queue has received service
 *
 * If a queue does not consume its budget fast enough, then providing
 * the queue with service fairness may impair throughput, more or less
 * severely. For this reason, queues that consume their budget slowly
 * are provided with time fairness instead of service fairness. This
 * goal is achieved through the BFQ scheduling engine, even if such an
 * engine works in the service, and not in the time domain. The trick
 * is charging these queues with an inflated amount of service, equal
 * to the amount of service that they would have received during their
 * service slot if they had been fast, i.e., if their requests had
 * been dispatched at a rate equal to the estimated peak rate.
 *
 * It is worth noting that time fairness can cause important
 * distortions in terms of bandwidth distribution, on devices with
 * internal queueing. The reason is that I/O requests dispatched
 * during the service slot of a queue may be served after that service
 * slot is finished, and may have a total processing time loosely
 * correlated with the duration of the service slot. This is
 * especially true for short service slots.
 */
static void bfq_bfqq_charge_time(struct bfq_data *bfqd, struct bfq_queue *bfqq,
				 unsigned long time_ms)
{
	struct bfq_entity *entity = &bfqq->entity;
	int tot_serv_to_charge = entity->service;
	unsigned int timeout_ms = jiffies_to_msecs(bfq_timeout);

	if (time_ms > 0 && time_ms < timeout_ms)
		tot_serv_to_charge =
			(bfqd->bfq_max_budget * time_ms) / timeout_ms;

	if (tot_serv_to_charge < entity->service)
		tot_serv_to_charge = entity->service;

	bfq_log_bfqq(bfqq->bfqd, bfqq,
		     "charge_time: %lu/%u ms, %d/%d/%d sectors",
		     time_ms, timeout_ms, entity->service,
		     tot_serv_to_charge, entity->budget);

	/* Increase budget to avoid inconsistencies */
	if (tot_serv_to_charge > entity->budget)
		entity->budget = tot_serv_to_charge;

	bfq_bfqq_served(bfqq,
			max_t(int, 0, tot_serv_to_charge - entity->service));
}

/**
 * __bfq_activate_entity - activate an entity.
 * @entity: the entity being activated.
 * @non_blocking_wait_rq: true if this entity was waiting for a request
 *
 * Called whenever an entity is activated, i.e., it is not active and one
 * of its children receives a new request, or has to be reactivated due to
 * budget exhaustion.  It uses the current budget of the entity (and the
 * service received if @entity is active) of the queue to calculate its
 * timestamps.
 */
static void __bfq_activate_entity(struct bfq_entity *entity,
				  bool non_blocking_wait_rq)
{
	struct bfq_sched_data *sd = entity->sched_data;
	struct bfq_service_tree *st = bfq_entity_service_tree(entity);
	struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
	bool backshifted = false;

	BUG_ON(!sd);
	BUG_ON(!st);
	if (entity == sd->in_service_entity) {
		BUG_ON(entity->tree);
		/*
		 * If we are requeueing the current entity we have
		 * to take care of not charging to it service it has
		 * not received.
		 */
		bfq_calc_finish(entity, entity->service);
		entity->start = entity->finish;
		sd->in_service_entity = NULL;
	} else if (entity->tree == &st->active) {
		/*
		 * Requeueing an entity due to a change of some
		 * next_in_service entity below it.  We reuse the
		 * old start time.
		 */
		bfq_active_extract(st, entity);
	} else {
		unsigned long long min_vstart;

		/* See comments on bfq_fqq_update_budg_for_activation */
		if (non_blocking_wait_rq && bfq_gt(st->vtime, entity->finish)) {
			backshifted = true;
			min_vstart = entity->finish;
		} else
			min_vstart = st->vtime;

		if (entity->tree == &st->idle) {
			/*
			 * Must be on the idle tree, bfq_idle_extract() will
			 * check for that.
			 */
			bfq_idle_extract(st, entity);
			entity->start = bfq_gt(min_vstart, entity->finish) ?
				min_vstart : entity->finish;
		} else {
			/*
			 * The finish time of the entity may be invalid, and
			 * it is in the past for sure, otherwise the queue
			 * would have been on the idle tree.
			 */
			entity->start = min_vstart;
			st->wsum += entity->weight;
			bfq_get_entity(entity);

			BUG_ON(entity->on_st);
			entity->on_st = 1;
		}
	}

	st = __bfq_entity_update_weight_prio(st, entity);
	bfq_calc_finish(entity, entity->budget);

	/*
	 * If some queues enjoy backshifting for a while, then their
	 * (virtual) finish timestamps may happen to become lower and
	 * lower than the system virtual time.  In particular, if
	 * these queues often happen to be idle for short time
	 * periods, and during such time periods other queues with
	 * higher timestamps happen to be busy, then the backshifted
	 * timestamps of the former queues can become much lower than
	 * the system virtual time. In fact, to serve the queues with
	 * higher timestamps while the ones with lower timestamps are
	 * idle, the system virtual time may be pushed-up to much
	 * higher values than the finish timestamps of the idle
	 * queues. As a consequence, the finish timestamps of all new
	 * or newly activated queues may end up being much larger than
	 * those of lucky queues with backshifted timestamps. The
	 * latter queues may then monopolize the device for a lot of
	 * time. This would simply break service guarantees.
	 *
	 * To reduce this problem, push up a little bit the
	 * backshifted timestamps of the queue associated with this
	 * entity (only a queue can happen to have the backshifted
	 * flag set): just enough to let the finish timestamp of the
	 * queue be equal to the current value of the system virtual
	 * time. This may introduce a little unfairness among queues
	 * with backshifted timestamps, but it does not break
	 * worst-case fairness guarantees.
	 *
	 * As a special case, if bfqq is weight-raised, push up
	 * timestamps much less, to keep very low the probability that
	 * this push up causes the backshifted finish timestamps of
	 * weight-raised queues to become higher than the backshifted
	 * finish timestamps of non weight-raised queues.
	 */
	if (backshifted && bfq_gt(st->vtime, entity->finish)) {
		unsigned long delta = st->vtime - entity->finish;

		if (bfqq)
			delta /= bfqq->wr_coeff;

		entity->start += delta;
		entity->finish += delta;

		if (bfqq) {
			bfq_log_bfqq(bfqq->bfqd, bfqq,
				     "__activate_entity: new queue finish %llu",
				     ((entity->finish>>10)*1000)>>12);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		} else {
			struct bfq_group *bfqg =
				container_of(entity, struct bfq_group, entity);

			bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
				     "__activate_entity: new group finish %llu",
				     ((entity->finish>>10)*1000)>>12);
#endif
		}
	}

	bfq_active_insert(st, entity);

	if (bfqq) {
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			"__activate_entity: queue %seligible in st %p",
			     entity->start <= st->vtime ? "" : "non ", st);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	} else {
		struct bfq_group *bfqg =
			container_of(entity, struct bfq_group, entity);

		bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
			"__activate_entity: group %seligible in st %p",
			     entity->start <= st->vtime ? "" : "non ", st);
#endif
	}
}

/**
 * bfq_activate_entity - activate an entity and its ancestors if necessary.
 * @entity: the entity to activate.
 * @non_blocking_wait_rq: true if this entity was waiting for a request
 *
 * Activate @entity and all the entities on the path from it to the root.
 */
static void bfq_activate_entity(struct bfq_entity *entity,
				bool non_blocking_wait_rq)
{
	struct bfq_sched_data *sd;

	for_each_entity(entity) {
		BUG_ON(!entity);
		__bfq_activate_entity(entity, non_blocking_wait_rq);

		sd = entity->sched_data;
		if (!bfq_update_next_in_service(sd))
			/*
			 * No need to propagate the activation to the
			 * upper entities, as they will be updated when
			 * the in-service entity is rescheduled.
			 */
			break;
	}
}

/**
 * __bfq_deactivate_entity - deactivate an entity from its service tree.
 * @entity: the entity to deactivate.
 * @requeue: if false, the entity will not be put into the idle tree.
 *
 * Deactivate an entity, independently from its previous state.  If the
 * entity was not on a service tree just return, otherwise if it is on
 * any scheduler tree, extract it from that tree, and if necessary
 * and if the caller did not specify @requeue, put it on the idle tree.
 *
 * Return %1 if the caller should update the entity hierarchy, i.e.,
 * if the entity was in service or if it was the next_in_service for
 * its sched_data; return %0 otherwise.
 */
static int __bfq_deactivate_entity(struct bfq_entity *entity, int requeue)
{
	struct bfq_sched_data *sd = entity->sched_data;
	struct bfq_service_tree *st;
	int was_in_service;
	int ret = 0;

	if (sd == NULL || !entity->on_st) /* never activated, or inactive */
		return 0;

	st = bfq_entity_service_tree(entity);
	was_in_service = entity == sd->in_service_entity;

	BUG_ON(was_in_service && entity->tree);

	if (was_in_service) {
		bfq_calc_finish(entity, entity->service);
		sd->in_service_entity = NULL;
	} else if (entity->tree == &st->active)
		bfq_active_extract(st, entity);
	else if (entity->tree == &st->idle)
		bfq_idle_extract(st, entity);
	else if (entity->tree)
		BUG();

	if (was_in_service || sd->next_in_service == entity)
		ret = bfq_update_next_in_service(sd);

	if (!requeue || !bfq_gt(entity->finish, st->vtime))
		bfq_forget_entity(st, entity);
	else
		bfq_idle_insert(st, entity);

	BUG_ON(sd->in_service_entity == entity);
	BUG_ON(sd->next_in_service == entity);

	return ret;
}

/**
 * bfq_deactivate_entity - deactivate an entity.
 * @entity: the entity to deactivate.
 * @requeue: true if the entity can be put on the idle tree
 */
static void bfq_deactivate_entity(struct bfq_entity *entity, int requeue)
{
	struct bfq_sched_data *sd;
	struct bfq_entity *parent;

	for_each_entity_safe(entity, parent) {
		sd = entity->sched_data;

		if (!__bfq_deactivate_entity(entity, requeue))
			/*
			 * next_in_service has not been changed, so
			 * no upwards update is needed
			 */
			break;

		if (sd->next_in_service)
			/*
			 * The parent entity is still backlogged,
			 * because next_in_service is not NULL, and
			 * next_in_service has been updated (see
			 * comment on the body of the above if):
			 * upwards update of the schedule is needed.
			 */
			goto update;

		/*
		 * If we get here, then the parent is no more backlogged and
		 * we want to propagate the deactivation upwards.
		 */
		requeue = 1;
	}

	return;

update:
	entity = parent;
	for_each_entity(entity) {
		struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);
		__bfq_activate_entity(entity, false);

		sd = entity->sched_data;
		if (bfqq)
			bfq_log_bfqq(bfqq->bfqd, bfqq,
				     "invoking udpdate_next for this queue");
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		else {
			struct bfq_group *bfqg =
				container_of(entity,
					     struct bfq_group, entity);

			bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
				     "invoking udpdate_next for this entity");
		}
#endif
		if (!bfq_update_next_in_service(sd))
			break;
	}
}

/**
 * bfq_update_vtime - update vtime if necessary.
 * @st: the service tree to act upon.
 *
 * If necessary update the service tree vtime to have at least one
 * eligible entity, skipping to its start time.  Assumes that the
 * active tree of the device is not empty.
 *
 * NOTE: this hierarchical implementation updates vtimes quite often,
 * we may end up with reactivated processes getting timestamps after a
 * vtime skip done because we needed a ->first_active entity on some
 * intermediate node.
 */
static void bfq_update_vtime(struct bfq_service_tree *st)
{
	struct bfq_entity *entry;
	struct rb_node *node = st->active.rb_node;

	entry = rb_entry(node, struct bfq_entity, rb_node);
	if (bfq_gt(entry->min_start, st->vtime)) {
		struct bfq_queue *bfqq = bfq_entity_to_bfqq(entry);
		st->vtime = entry->min_start;

		if (bfqq)
			bfq_log_bfqq(bfqq->bfqd, bfqq,
				     "update_vtime: new vtime %llu %p",
				     ((st->vtime>>10)*1000)>>12, st);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		else {
			struct bfq_group *bfqg =
				container_of(entry, struct bfq_group, entity);

			bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
				     "update_vtime: new vtime %llu %p",
				     ((st->vtime>>10)*1000)>>12, st);
		}
#endif
		bfq_forget_idle(st);
	}
}

/**
 * bfq_first_active_entity - find the eligible entity with
 *                           the smallest finish time
 * @st: the service tree to select from.
 *
 * This function searches the first schedulable entity, starting from the
 * root of the tree and going on the left every time on this side there is
 * a subtree with at least one eligible (start >= vtime) entity. The path on
 * the right is followed only if a) the left subtree contains no eligible
 * entities and b) no eligible entity has been found yet.
 */
static struct bfq_entity *bfq_first_active_entity(struct bfq_service_tree *st)
{
	struct bfq_entity *entry, *first = NULL;
	struct rb_node *node = st->active.rb_node;

	while (node) {
		entry = rb_entry(node, struct bfq_entity, rb_node);
left:
		if (!bfq_gt(entry->start, st->vtime))
			first = entry;

		BUG_ON(bfq_gt(entry->min_start, st->vtime));

		if (node->rb_left) {
			entry = rb_entry(node->rb_left,
					 struct bfq_entity, rb_node);
			if (!bfq_gt(entry->min_start, st->vtime)) {
				node = node->rb_left;
				goto left;
			}
		}
		if (first)
			break;
		node = node->rb_right;
	}

	BUG_ON(!first && !RB_EMPTY_ROOT(&st->active));
	return first;
}

/**
 * __bfq_lookup_next_entity - return the first eligible entity in @st.
 * @st: the service tree.
 *
 * Update the virtual time in @st and return the first eligible entity
 * it contains.
 */
static struct bfq_entity *
__bfq_lookup_next_entity(struct bfq_service_tree *st, bool force)
{
	struct bfq_entity *entity, *new_next_in_service = NULL;
	struct bfq_queue *bfqq;

	if (RB_EMPTY_ROOT(&st->active))
		return NULL;

	bfq_update_vtime(st);
	entity = bfq_first_active_entity(st);
	BUG_ON(bfq_gt(entity->start, st->vtime));

	bfqq = bfq_entity_to_bfqq(entity);
	if (bfqq)
		bfq_log_bfqq(bfqq->bfqd, bfqq,
			     "__lookup_next: start %llu vtime %llu st %p",
			     ((entity->start>>10)*1000)>>12,
			     ((st->vtime>>10)*1000)>>12, st);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
	else {
		struct bfq_group *bfqg =
			container_of(entity, struct bfq_group, entity);

		bfq_log_bfqg((struct bfq_data *)bfqg->bfqd, bfqg,
			     "__lookup_next: start %llu vtime %llu st %p",
			     ((entity->start>>10)*1000)>>12,
			     ((st->vtime>>10)*1000)>>12, st);
	}
#endif

	/*
	 * If the chosen entity does not match with the sched_data's
	 * next_in_service and we are forcedly serving the IDLE priority
	 * class tree, bubble up budget update.
	 */
	if (unlikely(force && entity != entity->sched_data->next_in_service)) {
		new_next_in_service = entity;
		for_each_entity(new_next_in_service)
			bfq_update_budget(new_next_in_service);
	}

	return entity;
}

/**
 * bfq_lookup_next_entity - return the first eligible entity in @sd.
 * @sd: the sched_data.
 * @extract: if true the returned entity will be also extracted from @sd.
 *
 * NOTE: since we cache the next_in_service entity at each level of the
 * hierarchy, the complexity of the lookup can be decreased with
 * absolutely no effort just returning the cached next_in_service value;
 * we prefer to do full lookups to test the consistency of * the data
 * structures.
 */
static struct bfq_entity *bfq_lookup_next_entity(struct bfq_sched_data *sd,
						 int extract,
						 struct bfq_data *bfqd)
{
	struct bfq_service_tree *st = sd->service_tree;
	struct bfq_entity *entity;
	int i = 0;

	BUG_ON(sd->in_service_entity);

	/*
	 * Choose from idle class, if needed to guarantee a minimum
	 * bandwidth to this class. This should also mitigate
	 * priority-inversion problems in case a low priority task is
	 * holding file system resources.
	 */
	if (bfqd &&
	    jiffies - bfqd->bfq_class_idle_last_service >
	    BFQ_CL_IDLE_TIMEOUT) {
		entity = __bfq_lookup_next_entity(st + BFQ_IOPRIO_CLASSES - 1,
						  true);
		if (entity) {
			struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

			if (bfqq)
				bfq_log_bfqq(bfqd, bfqq,
					     "idle chosen from st %p %d",
					     st + BFQ_IOPRIO_CLASSES - 1,
					BFQ_IOPRIO_CLASSES - 1);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
			else {
				struct bfq_group *bfqg =
				container_of(entity, struct bfq_group, entity);

				bfq_log_bfqg(bfqd, bfqg,
					     "idle chosen from st %p %d",
					     st + BFQ_IOPRIO_CLASSES - 1,
					BFQ_IOPRIO_CLASSES - 1);
			}
#endif
			i = BFQ_IOPRIO_CLASSES - 1;
			bfqd->bfq_class_idle_last_service = jiffies;
			sd->next_in_service = entity;
		}
	}
	for (; i < BFQ_IOPRIO_CLASSES; i++) {
		entity = __bfq_lookup_next_entity(st + i, false);
		if (entity) {
			if (bfqd != NULL) {
			struct bfq_queue *bfqq = bfq_entity_to_bfqq(entity);

			if (bfqq)
				bfq_log_bfqq(bfqd, bfqq,
					     "chosen from st %p %d",
					     st + i, i);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
			else {
				struct bfq_group *bfqg =
				container_of(entity, struct bfq_group, entity);

				bfq_log_bfqg(bfqd, bfqg,
					     "chosen from st %p %d",
					     st + i, i);
			}
#endif
			}

			if (extract) {
				bfq_check_next_in_service(sd, entity);
				bfq_active_extract(st + i, entity);
				sd->in_service_entity = entity;
				sd->next_in_service = NULL;
			}
			break;
		}
	}

	return entity;
}

static bool next_queue_may_preempt(struct bfq_data *bfqd)
{
	struct bfq_sched_data *sd = &bfqd->root_group->sched_data;

	return sd->next_in_service != sd->in_service_entity;
}

/*
 * Get next queue for service.
 */
static struct bfq_queue *bfq_get_next_queue(struct bfq_data *bfqd)
{
	struct bfq_entity *entity = NULL;
	struct bfq_sched_data *sd;
	struct bfq_queue *bfqq;

	BUG_ON(bfqd->in_service_queue);

	if (bfqd->busy_queues == 0)
		return NULL;

	sd = &bfqd->root_group->sched_data;
	for (; sd ; sd = entity->my_sched_data) {
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		if (entity) {
			struct bfq_group *bfqg =
				container_of(entity, struct bfq_group, entity);

			bfq_log_bfqg(bfqd, bfqg,
				     "get_next_queue: lookup in this group");
		} else
			bfq_log_bfqg(bfqd, bfqd->root_group,
				     "get_next_queue: lookup in root group");
#endif

		entity = bfq_lookup_next_entity(sd, 1, bfqd);

		bfqq = bfq_entity_to_bfqq(entity);
		if (bfqq)
			bfq_log_bfqq(bfqd, bfqq,
			     "get_next_queue: this queue, finish %llu",
				(((entity->finish>>10)*1000)>>10)>>2);
#ifdef CONFIG_BFQ_GROUP_IOSCHED
		else {
			struct bfq_group *bfqg =
				container_of(entity, struct bfq_group, entity);

			bfq_log_bfqg(bfqd, bfqg,
			     "get_next_queue: this entity, finish %llu",
				(((entity->finish>>10)*1000)>>10)>>2);
		}
#endif

		BUG_ON(!entity);
		entity->service = 0;
	}

	bfqq = bfq_entity_to_bfqq(entity);
	BUG_ON(!bfqq);

	return bfqq;
}

static void __bfq_bfqd_reset_in_service(struct bfq_data *bfqd)
{
	if (bfqd->in_service_bic) {
		put_io_context(bfqd->in_service_bic->icq.ioc);
		bfqd->in_service_bic = NULL;
	}

	bfq_clear_bfqq_wait_request(bfqd->in_service_queue);
	hrtimer_try_to_cancel(&bfqd->idle_slice_timer);
	bfqd->in_service_queue = NULL;
}

static void bfq_deactivate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq,
				int requeue)
{
	struct bfq_entity *entity = &bfqq->entity;

	BUG_ON(bfqq == bfqd->in_service_queue);
	bfq_deactivate_entity(entity, requeue);
}

static void bfq_activate_bfqq(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	struct bfq_entity *entity = &bfqq->entity;

	bfq_activate_entity(entity, bfq_bfqq_non_blocking_wait_rq(bfqq));
	bfq_clear_bfqq_non_blocking_wait_rq(bfqq);
}

static void bfqg_stats_update_dequeue(struct bfq_group *bfqg);

/*
 * Called when the bfqq no longer has requests pending, remove it from
 * the service tree.
 */
static void bfq_del_bfqq_busy(struct bfq_data *bfqd, struct bfq_queue *bfqq,
			      int requeue)
{
	BUG_ON(!bfq_bfqq_busy(bfqq));
	BUG_ON(!RB_EMPTY_ROOT(&bfqq->sort_list));
	BUG_ON(bfqq == bfqd->in_service_queue);

	bfq_log_bfqq(bfqd, bfqq, "del from busy");

	bfq_clear_bfqq_busy(bfqq);

	BUG_ON(bfqd->busy_queues == 0);
	bfqd->busy_queues--;

	if (!bfqq->dispatched)
		bfq_weights_tree_remove(bfqd, &bfqq->entity,
					&bfqd->queue_weights_tree);

	if (bfqq->wr_coeff > 1)
		bfqd->wr_busy_queues--;

	bfqg_stats_update_dequeue(bfqq_group(bfqq));

	BUG_ON(bfqq->entity.budget < 0);

	bfq_deactivate_bfqq(bfqd, bfqq, requeue);

	BUG_ON(bfqq->entity.budget < 0);
}

/*
 * Called when an inactive queue receives a new request.
 */
static void bfq_add_bfqq_busy(struct bfq_data *bfqd, struct bfq_queue *bfqq)
{
	BUG_ON(bfq_bfqq_busy(bfqq));
	BUG_ON(bfqq == bfqd->in_service_queue);

	bfq_log_bfqq(bfqd, bfqq, "add to busy");

	bfq_activate_bfqq(bfqd, bfqq);

	bfq_mark_bfqq_busy(bfqq);
	bfqd->busy_queues++;

	if (!bfqq->dispatched)
		if (bfqq->wr_coeff == 1)
			bfq_weights_tree_add(bfqd, &bfqq->entity,
					     &bfqd->queue_weights_tree);

	if (bfqq->wr_coeff > 1)
		bfqd->wr_busy_queues++;
}
