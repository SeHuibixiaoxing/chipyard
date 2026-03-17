import yaml


class CustomYamlDumper(yaml.Dumper):
    def ignore_aliases(self, data):
        """始终禁用锚点引用"""
        return True
    pass


def is_leaf_list(obj):
    """
    Check if the list contains only leaf nodes (non-dict, non-list).
    """
    return isinstance(obj, (list, tuple)) and all(not isinstance(item, (list, dict)) for item in obj)


def is_leaf_dict(obj):
    """
    Check if the dictionary contains only leaf nodes (non-dict, non-list values).
    """
    return isinstance(obj, dict) and all(not isinstance(value, (list, dict)) for value in obj.values())


def represent_list(dumper, data):
    """
    Custom representation for lists:
    - Use flow style if it's a leaf list.
    - Otherwise, use block style.
    """
    if is_leaf_list(data):
        return dumper.represent_sequence('tag:yaml.org,2002:seq', data, flow_style=True)
    return dumper.represent_sequence('tag:yaml.org,2002:seq', data, flow_style=False)


def represent_dict(dumper, data):
    """
    Custom representation for dictionaries:
    - Use flow style if it's a leaf dictionary.
    - Otherwise, use block style.
    """
    if is_leaf_dict(data):
        return dumper.represent_mapping('tag:yaml.org,2002:map', data, flow_style=True)
    return dumper.represent_mapping('tag:yaml.org,2002:map', data, flow_style=False)


def represent_tuple(dumper, data):
    """
    Custom representation for tuples to make them appear as regular sequences
    without the !!python/tuple tag.
    """
    if is_leaf_list(data):
        return dumper.represent_sequence('tag:yaml.org,2002:seq', data, flow_style=True)
    return dumper.represent_sequence('tag:yaml.org,2002:seq', data, flow_style=False)


# Add the custom representations to the dumper
CustomYamlDumper.add_representer(list, represent_list)
CustomYamlDumper.add_representer(dict, represent_dict)
CustomYamlDumper.add_representer(tuple, represent_tuple)
